/*
 * data.c — WDS data calls (multi-PDN).
 *
 * Wire layouts from the in-tree libqmi IDL:
 *   0xA2 Bind Mux Data Port : TLV 0x10 {u32 endpoint type, u32 iface},
 *                             TLV 0x11 mux id.
 *   0x20 Start Network      : TLV 0x14 APN, 0x16 user, 0x17 password,
 *                             0x18 auth, 0x19 IP family, 0x31 3GPP profile;
 *                             output TLV 0x01 packet data handle.
 *   0x2D Get Current Settings: output 0x1E IPv4, 0x20 gw, 0x21 mask, 0x25 IPv6,
 *                             0x26 v6 gw, 0x27/0x28 v6 DNS, 0x29 MTU,
 *                             0x2B/0x2C P-CSCF v4/v6.
 *   0x21 Stop Network       : TLV 0x01 handle.
 *   0x22 Get Packet Service Status; 0x02 Set Event Report (data call status).
 *
 * Stock runs two PDNs at once (default and IMS). Calls are kept in a table
 * keyed by cid, each on its own interface, and are never torn down by another
 * setup request.
 */
#define LOG_TAG "kebab-rild"

#include "data.h"
#include "ril_glue.h"
#include "state.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <log/log.h>

#define WDS_START_NETWORK      0x0020
#define WDS_STOP_NETWORK       0x0021
#define WDS_GET_SETTINGS       0x002D
#define WDS_SET_EVENT_REPORT   0x0002
#define WDS_GET_PKT_SRVC_STATUS 0x0022
#define WDS_BIND_MUX_DATA_PORT 0x00A2

#define DATA_DEFAULT_MTU 1400
#define DATA_MAX_MTU 1500

struct data_call {
    bool active;
    bool ready;
    int cid;
    uint8_t mux;
    uint32_t handle;
    char ifname[IFNAMSIZ];
    char addr[192];
    char gw[64];
    char dns[320];
    char pcscf[192];
    char type[8];
    int mtu;
    RIL_Data_Call_Response_v11 r;
};

static pthread_mutex_t s_data_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_op_mu = PTHREAD_MUTEX_INITIALIZER;
static struct data_call s_calls[DATA_MAX_CALLS];
static int s_next_cid = 1;
static char s_initial_apn[101];

static int ipv4_prefix(uint32_t mask)
{
    int n = 0;

    while (mask) {
        n += mask & 1u;
        mask >>= 1;
    }
    return n;
}

static int wds_bind_mux(uint8_t mux)
{
    uint8_t t[8];
    uint8_t req[24];
    size_t n = 0;

    t[0] = 3; t[1] = t[2] = t[3] = 0; /* endpoint type: PCIe = 3 */
    t[4] = 4; t[5] = t[6] = t[7] = 0; /* interface 4 (mhi_hwip0) */
    n = qmi_put_tlv(req, n, 0x10, t, sizeof(t));
    n = qmi_put_u8(req, n, 0x11, mux);
    return qmi_transact(&g_wds, WDS_BIND_MUX_DATA_PORT, req, n, NULL, NULL, 5000);
}

static void data_iface_up(const char *ifname, int mtu)
{
    int fd;
    struct ifreq ifr;

    if (!ifname || !ifname[0])
        return;
    if (mtu <= 0)
        mtu = DATA_DEFAULT_MTU;
    if (mtu > DATA_MAX_MTU)
        mtu = DATA_MAX_MTU;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(fd, SIOCSIFFLAGS, &ifr);
    }
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    ioctl(fd, SIOCSIFMTU, &ifr);
    close(fd);
}

static bool ifname_used_locked(const char *name)
{
    for (int i = 0; i < DATA_MAX_CALLS; i++)
        if (s_calls[i].active && !strcmp(s_calls[i].ifname, name))
            return true;
    return false;
}

/* One netdev per call. Mainline mhi_net may expose only mhi_hwip0; if a second
 * PDN is requested and no free netdev exists, setup fails instead of stealing
 * or tearing down the existing bearer. */
static bool pick_ifname_locked(char *out, size_t max)
{
    DIR *d = opendir("/sys/class/net");
    bool any = false;

    if (d) {
        struct dirent *e;

        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "mhi_hwip", 8))
                continue;
            any = true;
            if (!ifname_used_locked(e->d_name)) {
                snprintf(out, max, "%s", e->d_name);
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    if (!any) {
        char name[IFNAMSIZ];

        for (int i = 0; i < DATA_MAX_CALLS; i++) {
            snprintf(name, sizeof(name), "mhi_hwip%d", i);
            if (!ifname_used_locked(name)) {
                snprintf(out, max, "%s", name);
                return true;
            }
        }
    }
    return false;
}

static uint8_t ifname_mux(const char *name)
{
    const char *p = name + 8;

    return (uint8_t)(atoi(p) & 0x0f);
}

static int count_mhi_ifaces(void)
{
    DIR *d = opendir("/sys/class/net");
    int n = 0;

    if (!d)
        return DATA_MAX_CALLS;
    {
        struct dirent *e;

        while ((e = readdir(d)))
            if (!strncmp(e->d_name, "mhi_hwip", 8))
                n++;
    }
    closedir(d);
    /* No netdev yet: do not block setup on a probe race; the fallback
     * allocation path handles host builds and early boot. */
    return n ? n : DATA_MAX_CALLS;
}

static int alloc_cid_locked(void)
{
    for (int i = 0; i < 100; i++) {
        int cid = s_next_cid++;

        if (s_next_cid > 1000)
            s_next_cid = 1;
        for (int j = 0; j < DATA_MAX_CALLS; j++)
            if (s_calls[j].active && s_calls[j].cid == cid)
                goto next;
        return cid;
next:
        ;
    }
    return -1;
}

static struct data_call *slot_alloc_locked(void)
{
    for (int i = 0; i < DATA_MAX_CALLS; i++) {
        struct data_call *c = &s_calls[i];
        char name[IFNAMSIZ];
        int cid;

        if (c->active)
            continue;
        if (!pick_ifname_locked(name, sizeof(name)))
            return NULL;
        cid = alloc_cid_locked();
        if (cid < 0)
            return NULL;
        memset(c, 0, sizeof(*c));
        c->active = true;
        c->cid = cid;
        snprintf(c->ifname, sizeof(c->ifname), "%s", name);
        c->mux = ifname_mux(name);
        c->r.cid = cid;
        c->r.suggestedRetryTime = -1;
        return c;
    }
    return NULL;
}

static void slot_clear(struct data_call *c)
{
    memset(c, 0, sizeof(*c));
}

static void append_v6_dns(const uint8_t *v, uint16_t l, char *dns, size_t max)
{
    if (!v || l < 16)
        return;
    {
        uint8_t a6[16];
        char b[INET6_ADDRSTRLEN];
        size_t dl = strlen(dns);

        for (int w = 0; w < 8; w++) {
            a6[2 * w] = v[2 * w + 1];
            a6[2 * w + 1] = v[2 * w];
        }
        if (inet_ntop(AF_INET6, a6, b, sizeof(b)))
            snprintf(dns + dl, max - dl, "%s%s", dl ? " " : "", b);
    }
}

static void append_pcscf_v4(const uint8_t *v, uint16_t l, char *out, size_t max)
{
    if (!v)
        return;
    for (uint16_t off = 0; off + 4 <= l; off += 4) {
        size_t ol = strlen(out);

        if (ol + 1 >= max)
            return;
        snprintf(out + ol, max - ol, "%s%u.%u.%u.%u", ol ? " " : "",
                 v[off + 3], v[off + 2], v[off + 1], v[off]);
    }
}

static void append_pcscf_v6(const uint8_t *v, uint16_t l, char *out, size_t max)
{
    int ent = (l % 17 == 0) ? 17 : 16;

    if (!v)
        return;

    if (ent == 16 && l % 16)
        return;
    for (uint16_t off = 0; off + ent <= l; off += ent) {
        char b[INET6_ADDRSTRLEN];
        size_t ol = strlen(out);

        if (inet_ntop(AF_INET6, v + off + (ent == 17 ? 1 : 0), b, sizeof(b)))
            snprintf(out + ol, max - ol, "%s%s", ol ? " " : "", b);
    }
}

static void fill_settings(struct data_call *c, const uint8_t *tlvs, size_t len)
{
    uint16_t l = 0;
    const uint8_t *v;
    char addr[192] = "", gw[64] = "", dns[320] = "", pcscf[192] = "";
    int mtu = 0;
    int has_v4 = 0, has_v6 = 0;

    v = qmi_find_tlv(tlvs, len, 0x1E, &l);
    if (v && l >= 4) {
        char b[INET_ADDRSTRLEN];
        uint16_t ml = 0;
        const uint8_t *m;

        snprintf(b, sizeof(b), "%u.%u.%u.%u", v[3], v[2], v[1], v[0]);
        m = qmi_find_tlv(tlvs, len, 0x21, &ml);
        int pfx = (m && ml >= 4) ? ipv4_prefix((uint32_t)m[3] << 24 |
                                               (uint32_t)m[2] << 16 |
                                               (uint32_t)m[1] << 8 | m[0]) : 32;
        snprintf(addr, sizeof(addr), "%s/%d", b, pfx);
        has_v4 = 1;
    }
    v = qmi_find_tlv(tlvs, len, 0x25, &l);
    if (v && l >= 17) {
        char b[INET6_ADDRSTRLEN];

        if (inet_ntop(AF_INET6, v + 1, b, sizeof(b))) {
            size_t al = strlen(addr);

            snprintf(addr + al, sizeof(addr) - al, "%s%s/%u",
                     al ? " " : "", b, v[0]);
            has_v6 = 1;
        }
    }
    v = qmi_find_tlv(tlvs, len, 0x20, &l);
    if (v && l >= 4)
        snprintf(gw, sizeof(gw), "%u.%u.%u.%u", v[3], v[2], v[1], v[0]);

    v = qmi_find_tlv(tlvs, len, 0x15, &l); /* primary IPv4 DNS */
    if (v && l >= 4) {
        size_t dl = strlen(dns);

        snprintf(dns + dl, sizeof(dns) - dl, "%u.%u.%u.%u", v[3], v[2], v[1], v[0]);
    }
    v = qmi_find_tlv(tlvs, len, 0x16, &l); /* secondary IPv4 DNS */
    if (v && l >= 4) {
        size_t dl = strlen(dns);

        snprintf(dns + dl, sizeof(dns) - dl, "%s%u.%u.%u.%u", dl ? " " : "",
                 v[3], v[2], v[1], v[0]);
    }
    v = qmi_find_tlv(tlvs, len, 0x27, &l); /* primary IPv6 DNS */
    append_v6_dns(v, l, dns, sizeof(dns));
    v = qmi_find_tlv(tlvs, len, 0x28, &l); /* secondary IPv6 DNS */
    append_v6_dns(v, l, dns, sizeof(dns));

    v = qmi_find_tlv(tlvs, len, 0x2B, &l); /* P-CSCF IPv4 */
    append_pcscf_v4(v, l, pcscf, sizeof(pcscf));
    v = qmi_find_tlv(tlvs, len, 0x2C, &l); /* P-CSCF IPv6 */
    append_pcscf_v6(v, l, pcscf, sizeof(pcscf));

    v = qmi_find_tlv(tlvs, len, 0x29, &l);
    if (v && l >= 4)
        mtu = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    if (mtu <= 0)
        mtu = DATA_DEFAULT_MTU;
    else if (mtu > DATA_MAX_MTU)
        mtu = DATA_MAX_MTU;

    snprintf(c->addr, sizeof(c->addr), "%s", addr);
    snprintf(c->gw, sizeof(c->gw), "%s", gw);
    snprintf(c->dns, sizeof(c->dns), "%s", dns);
    snprintf(c->pcscf, sizeof(c->pcscf), "%s", pcscf);
    if (has_v4 && has_v6)
        snprintf(c->type, sizeof(c->type), "IPV4V6");
    else if (has_v6)
        snprintf(c->type, sizeof(c->type), "IPV6");
    else
        snprintf(c->type, sizeof(c->type), "IP");
    c->mtu = mtu;
    c->r.status = 0;
    c->r.active = 1;
    c->r.cid = c->cid;
    c->r.suggestedRetryTime = -1;
    c->r.ifname = c->ifname;
    c->r.type = c->type;
    c->r.addresses = c->addr;
    c->r.dnses = c->dns;
    c->r.gateways = c->gw;
    c->r.pcscf = c->pcscf;
    c->r.mtu = mtu;
}

void data_set_initial_attach_apn(const char *apn)
{
    pthread_mutex_lock(&s_data_mu);
    snprintf(s_initial_apn, sizeof(s_initial_apn), "%s",
             apn && apn[0] ? apn : "");
    pthread_mutex_unlock(&s_data_mu);
}

int data_setup(const char *apn, const char *pdp_type, const char *roaming_type,
               const char *user, const char *password, int auth, int profile,
               RIL_Data_Call_Response_v11 *out)
{
    uint8_t req[512], rsp[1024];
    size_t n = 0, rl = sizeof(rsp);
    uint8_t fams[2];
    int nfam;
    const char *eff = pdp_type;
    char apn_buf[101];
    bool roaming;
    struct data_call *c;
    uint16_t l = 0;
    const uint8_t *v;
    int rc;
    uint32_t handle = 0;

    memset(apn_buf, 0, sizeof(apn_buf));
    pthread_mutex_lock(&g_state.mu);
    roaming = g_state.roaming;
    pthread_mutex_unlock(&g_state.mu);
    if (roaming && roaming_type && roaming_type[0])
        eff = roaming_type;

    pthread_mutex_lock(&s_data_mu);
    snprintf(apn_buf, sizeof(apn_buf), "%s",
             apn && apn[0] ? apn : s_initial_apn);
    pthread_mutex_unlock(&s_data_mu);

    if (eff && !strcmp(eff, "IPV6")) {
        fams[0] = 6;
        nfam = 1;
    } else if (eff && !strcmp(eff, "IPV4V6")) {
        /* IPv4 first, then IPv6: stock issues both families separately. */
        fams[0] = 4;
        fams[1] = 6;
        nfam = 2;
    } else {
        fams[0] = 4;
        nfam = 1;
    }
    if (!apn_buf[0]) {
        ALOGE("DATA: empty APN");
        return -EINVAL;
    }
    /* With a single mhi_net interface only one PDN can exist. Keep it for the
     * default bearer: an IMS PDN request must not take the only netdev and
     * leave internet without one. */
    if (!strcmp(apn_buf, "ims") && count_mhi_ifaces() < 2) {
        ALOGE("DATA: IMS PDN refused (single data netdev)");
        return -EBUSY;
    }

    pthread_mutex_lock(&s_op_mu);
    if (!qmi_client_is_up(&g_wds) && qmi_client_wait_up(&g_wds, 20000)) {
        pthread_mutex_unlock(&s_op_mu);
        return -ENOTCONN;
    }

    pthread_mutex_lock(&s_data_mu);
    c = slot_alloc_locked();
    pthread_mutex_unlock(&s_data_mu);
    if (!c) {
        pthread_mutex_unlock(&s_op_mu);
        ALOGE("DATA: no free PDN slot/interface");
        return -EBUSY;
    }

    {
        int brc = wds_bind_mux(c->mux);

        if (brc)
            ALOGE("DATA: bind mux %u failed %d", c->mux, brc);
    }

    for (int fi = 0; fi < nfam; fi++) {
        n = 0;
        n = qmi_put_tlv(req, n, 0x14, apn_buf, (uint16_t)strlen(apn_buf));
        n = qmi_put_u8(req, n, 0x19, fams[fi]);
        if (profile > 0)
            n = qmi_put_u8(req, n, 0x31, (uint8_t)profile);
        if (user && user[0])
            n = qmi_put_tlv(req, n, 0x16, user, (uint16_t)strlen(user));
        if (password && password[0])
            n = qmi_put_tlv(req, n, 0x17, password,
                            (uint16_t)strlen(password));
        if (auth > 0)
            n = qmi_put_u8(req, n, 0x18, (uint8_t)auth);

        rl = sizeof(rsp);
        rc = qmi_transact(&g_wds, WDS_START_NETWORK, req, n, rsp, &rl, 60000);
        if (rc) {
            if (fi + 1 < nfam) {
                ALOGI("DATA: family %u start failed %d, trying next",
                      fams[fi], rc);
                continue;
            }
            ALOGE("DATA: start '%s' failed %d", apn_buf, rc);
            pthread_mutex_lock(&s_data_mu);
            slot_clear(c);
            pthread_mutex_unlock(&s_data_mu);
            pthread_mutex_unlock(&s_op_mu);
            return rc;
        }
        v = qmi_find_tlv(rsp, rl, 0x01, &l);
        if (!v || l < 4) {
            ALOGE("DATA: no packet data handle");
            pthread_mutex_lock(&s_data_mu);
            slot_clear(c);
            pthread_mutex_unlock(&s_data_mu);
            pthread_mutex_unlock(&s_op_mu);
            return -EIO;
        }
        handle = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                 ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
        pthread_mutex_lock(&s_data_mu);
        c->handle = handle;
        pthread_mutex_unlock(&s_data_mu);

        {
            uint8_t sreq[16];
            size_t sn = qmi_put_u32(sreq, 0, 0x10, 0xffffffff);

            rl = sizeof(rsp);
            rc = qmi_transact(&g_wds, WDS_GET_SETTINGS, sreq, sn, rsp, &rl,
                              10000);
        }
        if (rc) {
            uint8_t sreq[8];
            size_t sn = qmi_put_u32(sreq, 0, 0x01, handle);

            ALOGE("DATA: settings family %u failed %d", fams[fi], rc);
            qmi_transact(&g_wds, WDS_STOP_NETWORK, sreq, sn, NULL, NULL,
                         10000);
            pthread_mutex_lock(&s_data_mu);
            c->handle = 0;
            pthread_mutex_unlock(&s_data_mu);
            if (fi + 1 < nfam)
                continue;
            pthread_mutex_lock(&s_data_mu);
            slot_clear(c);
            pthread_mutex_unlock(&s_data_mu);
            pthread_mutex_unlock(&s_op_mu);
            return rc > 0 ? -EIO : rc;
        }

        pthread_mutex_lock(&s_data_mu);
        fill_settings(c, rsp, rl);
        if (!c->addr[0] && fi + 1 < nfam) {
            uint8_t sreq[8];
            size_t sn = qmi_put_u32(sreq, 0, 0x01, handle);

            pthread_mutex_unlock(&s_data_mu);
            qmi_transact(&g_wds, WDS_STOP_NETWORK, sreq, sn, NULL, NULL,
                         10000);
            pthread_mutex_lock(&s_data_mu);
            c->handle = 0;
            pthread_mutex_unlock(&s_data_mu);
            ALOGI("DATA: family %u returned no address, trying next",
                  fams[fi]);
            continue;
        }
        c->ready = true;
        if (out)
            *out = c->r;
        pthread_mutex_unlock(&s_data_mu);
        break;
    }

    data_iface_up(c->ifname, c->mtu);
    ALOGI("DATA: cid %d mux %u if %s apn '%s' addr '%s' mtu %d", c->cid,
          c->mux, c->ifname, apn_buf, c->addr, c->mtu);
    pthread_mutex_unlock(&s_op_mu);
    return 0;
}

int data_deactivate_cid(int cid)
{
    struct data_call *c = NULL;
    uint32_t handle = 0;
    int rc = 0;

    pthread_mutex_lock(&s_op_mu);
    pthread_mutex_lock(&s_data_mu);
    for (int i = 0; i < DATA_MAX_CALLS; i++) {
        if (s_calls[i].active && s_calls[i].cid == cid) {
            c = &s_calls[i];
            break;
        }
    }
    if (c) {
        handle = c->handle;
        c->handle = 0;
    }
    pthread_mutex_unlock(&s_data_mu);
    if (!c) {
        pthread_mutex_unlock(&s_op_mu);
        return -ENOENT;
    }
    if (handle) {
        uint8_t req[8];
        size_t n = qmi_put_u32(req, 0, 0x01, handle);

        rc = qmi_transact(&g_wds, WDS_STOP_NETWORK, req, n, NULL, NULL, 10000);
        if (rc)
            ALOGE("DATA: stop cid %d failed %d", cid, rc);
    }
    pthread_mutex_lock(&s_data_mu);
    slot_clear(c);
    pthread_mutex_unlock(&s_data_mu);
    pthread_mutex_unlock(&s_op_mu);
    return rc;
}

void data_deactivate_all(void)
{
    pthread_mutex_lock(&s_op_mu);
    for (int i = 0; i < DATA_MAX_CALLS; i++) {
        struct data_call *c = &s_calls[i];
        uint32_t handle;

        pthread_mutex_lock(&s_data_mu);
        handle = c->active && c->ready ? c->handle : 0;
        pthread_mutex_unlock(&s_data_mu);
        if (!handle)
            continue;
        {
            uint8_t req[8];
            size_t n = qmi_put_u32(req, 0, 0x01, handle);

            qmi_transact(&g_wds, WDS_STOP_NETWORK, req, n, NULL, NULL, 10000);
        }
        pthread_mutex_lock(&s_data_mu);
        slot_clear(c);
        pthread_mutex_unlock(&s_data_mu);
    }
    pthread_mutex_unlock(&s_op_mu);
}

void data_clear_all(void)
{
    pthread_mutex_lock(&s_data_mu);
    for (int i = 0; i < DATA_MAX_CALLS; i++)
        slot_clear(&s_calls[i]);
    pthread_mutex_unlock(&s_data_mu);
}

int data_snapshot_all(RIL_Data_Call_Response_v11 *out, int max)
{
    int n = 0;

    if (!out || max <= 0)
        return 0;
    pthread_mutex_lock(&s_data_mu);
    for (int i = 0; i < DATA_MAX_CALLS && n < max; i++) {
        if (s_calls[i].active && s_calls[i].ready)
            out[n++] = s_calls[i].r;
    }
    pthread_mutex_unlock(&s_data_mu);
    return n;
}

static volatile bool s_chk_running;
static pthread_mutex_t s_chk_mu = PTHREAD_MUTEX_INITIALIZER;

/* Modem-initiated deactivation: compare the modem's own status instead of
 * trusting the cached snapshot. Never runs on the client thread. */
static void *data_status_thread(void *arg)
{
    uint8_t rsp[128];
    size_t rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;

    (void)arg;
    if (qmi_transact(&g_wds, WDS_GET_PKT_SRVC_STATUS, NULL, 0, rsp, &rl,
                     5000) != 0)
        goto out;
    v = qmi_find_tlv(rsp, rl, 0x01, &l);
    if (v && l >= 1 && v[0] == 0) { /* disconnected */
        bool had = false;

        pthread_mutex_lock(&s_data_mu);
        for (int i = 0; i < DATA_MAX_CALLS; i++)
            had = had || s_calls[i].active;
        pthread_mutex_unlock(&s_data_mu);
        if (had) {
            data_clear_all();
            kebab_unsol(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
        }
    }
out:
    pthread_mutex_lock(&s_chk_mu);
    s_chk_running = false;
    pthread_mutex_unlock(&s_chk_mu);
    return NULL;
}

static void data_spawn_status_check(void)
{
    pthread_t t;

    pthread_mutex_lock(&s_chk_mu);
    if (s_chk_running) {
        pthread_mutex_unlock(&s_chk_mu);
        return;
    }
    s_chk_running = true;
    pthread_mutex_unlock(&s_chk_mu);
    if (pthread_create(&t, NULL, data_status_thread, NULL) != 0) {
        pthread_mutex_lock(&s_chk_mu);
        s_chk_running = false;
        pthread_mutex_unlock(&s_chk_mu);
        return;
    }
    pthread_detach(t);
}

static void *data_register_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10 && qmi_client_is_up(&g_wds); i++) {
        uint8_t req[8];
        size_t n = qmi_put_u8(req, 0, 0x10, 1); /* report data call status */
        int rc = qmi_transact(&g_wds, WDS_SET_EVENT_REPORT, req, n, NULL,
                              NULL, 5000);

        if (rc == 0)
            return NULL;
        sleep(1);
    }
    return NULL;
}

static void data_ind_cb(struct qmi_client *c, uint16_t msg,
                        const uint8_t *tlvs, size_t len)
{
    (void)c;
    (void)tlvs;
    (void)len;
    ALOGD("DATA: indication 0x%04x", msg);
    if (msg == 0x0001 || msg == 0x0010)
        data_spawn_status_check();
}

static void data_up_cb(struct qmi_client *c, bool up)
{
    pthread_t t;

    (void)c;
    ALOGI("DATA: WDS %s", up ? "up" : "down");
    if (up) {
        if (pthread_create(&t, NULL, data_register_thread, NULL) == 0)
            pthread_detach(t);
        return;
    }
    data_clear_all();
    kebab_unsol(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
}

void data_init(void)
{
    if (qmi_client_start(&g_wds, QMI_SVC_WDS, data_ind_cb, data_up_cb) != 0)
        ALOGE("DATA: client start failed");
}
