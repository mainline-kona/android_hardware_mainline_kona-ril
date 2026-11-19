/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * kebab-dataconn <apn> <ipv4|ipv6>
 *
 * Brings up one mobile data session on the external SDX55 (QMI WDS/WDA over
 * QRTR, or raw QMUX on /dev/wwan0qmi0 with KEBAB_QMI_TRANSPORT=qmux; raw-IP
 * data on the mhi_net IP_HW0 netdev) and holds it
 * until SIGTERM. Speaks the same stdout protocol as phone2's pong-dataconn:
 * key=value lines (ifname, address, gateway, dns, mtu), then "ready".
 * On failure prints "end_reason=..."/"error=..." and exits non-zero; when
 * the modem drops the call it prints "disconnected" and exits.
 *
 * No libqmi/GLib: the handful of messages needed are built by hand.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/qrtr.h>

#ifndef IFLA_RMNET_MUX_ID
#define IFLA_RMNET_MUX_ID 1
#endif
#ifndef __BIONIC__
#include <linux/ipv6.h> /* struct in6_ifreq; bionic has it in netinet/in.h */
#endif

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QMI_DEV_DEFAULT "/dev/wwan0qmi0"

#define SVC_CTL 0x00
#define SVC_WDS 0x01
#define SVC_WDA 0x1a
#define SVC_DMS 0x02

#define DMS_GET_CAPABILITIES 0x0020
#define DMS_GET_REVISION 0x0023
#define DMS_GET_OPERATING_MODE 0x002d
#define DMS_SET_OPERATING_MODE 0x002e

#define CTL_GET_CLIENT_ID 0x0022
#define CTL_RELEASE_CLIENT_ID 0x0023
#define WDA_SET_DATA_FORMAT 0x0020
#define WDS_START_NETWORK 0x0020
#define WDS_STOP_NETWORK 0x0021
#define WDS_PKT_SRVC_STATUS 0x0022
#define WDS_GET_CURRENT_SETTINGS 0x002d
#define WDS_SET_IP_FAMILY 0x004d
#define WDS_BIND_MUX_DATA_PORT 0x00a2

static int s_fd = -1;
static uint16_t s_txn = 1;
static uint8_t s_ctl_txn = 1;
static uint8_t s_wds_cid, s_wda_cid;
static volatile sig_atomic_t s_stop;
static int s_disconnected;
static int s_abortable; /* SIGTERM cancels the request in flight (Start Network) */
static int s_qmap;      /* opt-in QMAP pass-through: KEBAB_QMAP=1 */
static unsigned int s_qmap_mux = 1;

/*
 * QRTR transport. The X55's QMUX port (/dev/wwan0qmi0) never answers CTL, but
 * its QMI services are all published on QRTR (over the MHI IPCR channel), as
 * the stock stack uses them. Over QRTR there is no QMUX header and no CTL: each
 * socket is its own client of one service, addressed by (node, port).
 */
static int s_qrtr;
struct qclient { int fd; uint32_t node, port; };
static struct qclient s_qwds = {-1, 0, 0}, s_qwda = {-1, 0, 0};

static struct qclient *qclient_for(uint8_t svc) {
    if (!s_qrtr) return NULL;
    if (svc == SVC_WDS) return &s_qwds;
    if (svc == SVC_WDA) return &s_qwda;
    return NULL;
}

/* ---------- message building ---------- */

struct msg {
    uint8_t buf[2048];
    size_t len;     /* bytes used */
    size_t tlv_off; /* offset of the SDU's TLV length field */
    int over;       /* set when a field would overflow buf */
};

static void put8(struct msg *m, uint8_t v) {
    if (m->len >= sizeof(m->buf)) {
        m->over = 1;
        return;
    }
    m->buf[m->len++] = v;
}
static void put16(struct msg *m, uint16_t v) { put8(m, v & 0xff); put8(m, v >> 8); }

static uint16_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

static void msg_init(struct msg *m, uint8_t svc, uint8_t cid, uint16_t msgid, uint16_t *txn_out) {
    m->len = 0;
    m->over = 0;
    put8(m, 0x01);      /* QMUX interface type */
    put16(m, 0);        /* length, patched in msg_send */
    put8(m, 0x00);      /* control flags: from host */
    put8(m, svc);
    put8(m, cid);
    put8(m, 0x00);      /* SDU flags: request */
    if (svc == SVC_CTL) {
        if (!++s_ctl_txn) s_ctl_txn = 1;
        put8(m, s_ctl_txn);
        *txn_out = s_ctl_txn;
    } else {
        if (!++s_txn) s_txn = 1;
        put16(m, s_txn);
        *txn_out = s_txn;
    }
    put16(m, msgid);
    m->tlv_off = m->len;
    put16(m, 0);
}

static void tlv(struct msg *m, uint8_t type, const void *data, uint16_t len) {
    if (m->over || m->len + 3 + len > sizeof(m->buf)) {
        m->over = 1;
        return;
    }
    put8(m, type);
    put16(m, len);
    memcpy(m->buf + m->len, data, len);
    m->len += len;
}
static void tlv8(struct msg *m, uint8_t type, uint8_t v) { tlv(m, type, &v, 1); }
static void tlv32(struct msg *m, uint8_t type, uint32_t v) {
    uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    tlv(m, type, b, 4);
}

/* ---------- transport ---------- */

struct resp {
    uint8_t buf[4096];
    size_t len;
    const uint8_t *tlvs;
    size_t tlvs_len;
    uint16_t result, error;
};

static void dump_tlvs(const char *what, const struct resp *r);

static const uint8_t *find_tlv(const struct resp *r, uint8_t type, uint16_t *len) {
    size_t off = 0;
    while (off + 3 <= r->tlvs_len) {
        uint8_t t = r->tlvs[off];
        uint16_t l = get16(r->tlvs + off + 1);
        if (off + 3 + l > r->tlvs_len) break;
        if (t == type) {
            if (len) *len = l;
            return r->tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

/* Handle an unsolicited indication; sets s_disconnected when the data call drops */
static void handle_indication(uint8_t svc, uint16_t msgid, struct resp *r) {
    if (svc == SVC_WDS && msgid == WDS_PKT_SRVC_STATUS) {
        uint16_t l;
        const uint8_t *v = find_tlv(r, 0x01, &l);
        if (v && l >= 1) printf("info=wds packet status %u\n", v[0]);
        if (v && l >= 1 && v[0] == 1 /* disconnected */) {
            dump_tlvs("pkt-status", r);
            s_disconnected = 1;
            const uint8_t *er = find_tlv(r, 0x10, &l);
            if (er && l >= 2) printf("end_reason=%u\n", get16(er));
            printf("disconnected\n");
            fflush(stdout);
        }
    }
}

/*
 * Read one QMUX message. Returns 1 for a parsed message (svc/msgid/type
 * filled), 0 on timeout, -1 on error.
 */
static int read_msg(struct resp *r, int timeout_ms, uint8_t *svc, uint8_t *cid,
                    uint16_t *msgid, uint16_t *txn, uint8_t *type) {
    struct pollfd pfd = {.fd = s_fd, .events = POLLIN};
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret < 0 && errno != EINTR ? -1 : 0;
    ssize_t n = read(s_fd, r->buf, sizeof(r->buf));
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n < 6 || r->buf[0] != 0x01) return 0;
    r->len = n;
    *svc = r->buf[4];
    *cid = r->buf[5];
    size_t off = 6;
    uint8_t flags = r->buf[off++];
    if (*svc == SVC_CTL) {
        if ((size_t)n < off + 5) return 0;
        *txn = r->buf[off++];
        /* CTL flags: bit0 response, bit1 indication */
        *type = (flags & 0x02) ? 4 : ((flags & 0x01) ? 2 : 0);
    } else {
        if ((size_t)n < off + 6) return 0;
        *txn = get16(r->buf + off);
        off += 2;
        /* service flags: bit1 response, bit2 indication */
        *type = (flags & 0x04) ? 4 : ((flags & 0x02) ? 2 : 0);
    }
    *msgid = get16(r->buf + off);
    uint16_t tl = get16(r->buf + off + 2);
    off += 4;
    if (off + tl > (size_t)n) tl = n - off;
    r->tlvs = r->buf + off;
    r->tlvs_len = tl;
    r->result = r->error = 0xffff;
    uint16_t l;
    const uint8_t *res = find_tlv(r, 0x02, &l);
    if (res && l >= 4) {
        r->result = get16(res);
        r->error = get16(res + 2);
    }
    return 1;
}

/* Read one QMI SDU from a QRTR client socket; same contract as read_msg() */
static int qrtr_read_msg(struct qclient *c, struct resp *r, int timeout_ms, uint16_t *msgid,
                         uint16_t *txn, uint8_t *type) {
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret < 0 && errno != EINTR ? -1 : 0;
    struct sockaddr_qrtr from;
    socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(c->fd, r->buf, sizeof(r->buf), 0, (void *)&from, &fl);
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n < 7 || from.sq_node != c->node || from.sq_port != c->port) return 0;
    r->len = n;
    /* QMI flags: 0 request, 2 response, 4 indication */
    *type = r->buf[0] & 0x06;
    *txn = get16(r->buf + 1);
    *msgid = get16(r->buf + 3);
    uint16_t tl = get16(r->buf + 5);
    r->tlvs = r->buf + 7;
    r->tlvs_len = (size_t)n - 7 < tl ? (size_t)n - 7 : tl;
    r->result = r->error = 0xffff;
    uint16_t l;
    const uint8_t *res = find_tlv(r, 0x02, &l);
    if (res && l >= 4) {
        r->result = get16(res);
        r->error = get16(res + 2);
    }
    return 1;
}

static int msg_send(struct msg *m) {
    if (m->over)
        return -EMSGSIZE;
    uint16_t qmux_len = m->len - 1;
    m->buf[1] = qmux_len & 0xff;
    m->buf[2] = qmux_len >> 8;
    uint16_t tl = m->len - m->tlv_off - 2;
    m->buf[m->tlv_off] = tl & 0xff;
    m->buf[m->tlv_off + 1] = tl >> 8;
    return write(s_fd, m->buf, m->len) == (ssize_t)m->len ? 0 : -1;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* KEBAB_QMI_TRACE=0 disables the frame dumps (set by the RIL from
 * persist.vendor.radio.qmi_trace, or the service's environment). */
static int trace_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KEBAB_QMI_TRACE");
        on = !e || *e != '0';
    }
    return on;
}

static void dump_frame(const char *what, const uint8_t *p, size_t n) {
    char line[3 * 16 + 1];
    if (!trace_on()) return;
    printf("trace=%s len %zu\n", what, n);
    for (size_t off = 0; off < n && off < 512; off += 16) {
        size_t chunk = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < chunk; i++) sprintf(line + 3 * i, " %02x", p[off + i]);
        line[3 * chunk] = 0;
        printf("trace=  %03zx:%s\n", off, line);
    }
    if (n > 512) printf("trace=  ... %zu more bytes\n", n - 512);
}

/* Send and wait for the matching response. Returns 0 if QMI result is success. */
static int transact(struct msg *m, uint8_t svc, uint16_t msgid, uint16_t txn, struct resp *r,
                    int timeout_ms) {
    uint8_t cid = m->buf[5];
    struct qclient *qc = qclient_for(svc);
    r->result = r->error = 0xffff;
    r->tlvs_len = 0;
    if (qc) {
        uint16_t tl = m->len - m->tlv_off - 2;
        m->buf[m->tlv_off] = tl & 0xff;
        m->buf[m->tlv_off + 1] = tl >> 8;
        struct sockaddr_qrtr sq = {.sq_family = AF_QIPCRTR, .sq_node = qc->node,
                                   .sq_port = qc->port};
        /* the SDU is the QMUX message minus its 6-byte header */
        dump_frame("qmi tx", m->buf + 6, m->len - 6);
        if (sendto(qc->fd, m->buf + 6, m->len - 6, 0, (void *)&sq, sizeof(sq)) < 0) {
            printf("error=qrtr send svc 0x%02x: %s\n", svc, strerror(errno));
            return -1;
        }
    } else if (msg_send(m) < 0) {
        printf("error=write %s: %s\n", QMI_DEV_DEFAULT, strerror(errno));
        return -1;
    } else {
        dump_frame("qmi tx", m->buf, m->len);
    }
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (s_abortable && s_stop) {
            printf("warning=cancelled by SIGTERM: svc 0x%02x msg 0x%04x\n", svc, msgid);
            return -1;
        }
        if (left <= 0) {
            printf("warning=timeout waiting for svc 0x%02x msg 0x%04x\n", svc, msgid);
            return -1;
        }
        uint8_t rsvc = svc, rcid = cid, type;
        uint16_t rmsg, rtxn;
        int ret = qc ? qrtr_read_msg(qc, r, left < 1000 ? left : 1000, &rmsg, &rtxn, &type)
                     : read_msg(r, left < 1000 ? left : 1000, &rsvc, &rcid, &rmsg, &rtxn, &type);
        if (ret < 0) {
            printf("error=read: %s\n", strerror(errno));
            return -1;
        }
        if (ret == 0) continue;
        if (type == 4) {
            dump_frame("qmi ind", r->buf, r->len);
            handle_indication(rsvc, rmsg, r);
            continue;
        }
        if (type == 2 && rsvc == svc && rcid == cid && rmsg == msgid && rtxn == txn) {
            dump_frame("qmi rx", r->buf, r->len);
            printf("info=qmi svc 0x%02x msg 0x%04x txn %u result %u error %u len %zu\n", svc,
                   msgid, txn, r->result, r->error, r->len);
            return r->result == 0 ? 0 : -1;
        }
    }
}

/* ---------- QMI operations ---------- */

static int ctl_get_cid(uint8_t svc, uint8_t *cid) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    msg_init(&m, SVC_CTL, 0, CTL_GET_CLIENT_ID, &txn);
    tlv8(&m, 0x01, svc);
    if (transact(&m, SVC_CTL, CTL_GET_CLIENT_ID, txn, &r, 10000)) {
        printf("warning=allocate client for svc 0x%02x: error %u\n", svc, r.error);
        return -1;
    }
    uint16_t l;
    const uint8_t *v = find_tlv(&r, 0x01, &l);
    if (!v || l < 2) return -1;
    *cid = v[1];
    return 0;
}

static void ctl_release_cid(uint8_t svc, uint8_t cid) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    uint8_t v[2] = {svc, cid};
    if (!cid) return;
    msg_init(&m, SVC_CTL, 0, CTL_RELEASE_CLIENT_ID, &txn);
    tlv(&m, 0x01, v, 2);
    transact(&m, SVC_CTL, CTL_RELEASE_CLIENT_ID, txn, &r, 3000);
}

/* Raw IP, no aggregation: matches mhi_net's plain IP_HW0 netdev */
static int wda_set_raw_ip(int with_endpoint) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    msg_init(&m, SVC_WDA, s_wda_cid, WDA_SET_DATA_FORMAT, &txn);
    tlv32(&m, 0x11, 2);  /* link layer protocol: raw IP */
    tlv32(&m, 0x12, s_qmap ? 5 : 0);  /* UL aggregation: QMAP or disabled */
    tlv32(&m, 0x13, s_qmap ? 5 : 0);  /* DL aggregation: QMAP or disabled */
    if (with_endpoint) {
        uint8_t ep[8] = {3, 0, 0, 0, 4, 0, 0, 0};  /* PCIe, interface 4 (as ModemManager) */
        tlv(&m, 0x17, ep, sizeof(ep));
    }
    if (transact(&m, SVC_WDA, WDA_SET_DATA_FORMAT, txn, &r, 10000)) {
        printf("warning=wda set data format (endpoint %d): error %u\n", with_endpoint, r.error);
        return -1;
    }
    uint16_t l;
    const uint8_t *v = find_tlv(&r, 0x11, &l);
    if (v && l >= 4 && get32(v) != 2) printf("warning=modem kept link layer %u\n", get32(v));
    return 0;
}

static void wds_set_family(uint8_t fam) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_SET_IP_FAMILY, &txn);
    tlv8(&m, 0x01, fam);
    transact(&m, SVC_WDS, WDS_SET_IP_FAMILY, txn, &r, 5000);
}

/*
 * Over QRTR the modem cannot tell which data pipe a WDS client means (on the
 * QMUX port that is implied by the port itself), so bind it to the PCIe
 * endpoint that carries IP_HW0, as the stock netmgr does (ep_type 3).
 * mux id 0: plain raw-IP netdev, no QMAP/rmnet on top.
 */
static int wds_bind_data_port(uint32_t ep_iface, uint8_t mux_id) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    uint8_t ep[8] = {3, 0, 0, 0, ep_iface, ep_iface >> 8, ep_iface >> 16, ep_iface >> 24};
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_BIND_MUX_DATA_PORT, &txn);
    tlv(&m, 0x10, ep, sizeof(ep));
    tlv8(&m, 0x11, mux_id);
    if (transact(&m, SVC_WDS, WDS_BIND_MUX_DATA_PORT, txn, &r, 5000)) {
        printf("warning=wds bind data port (iface %u mux %u): result %u error %u\n", ep_iface,
               mux_id, r.result, r.error);
        return -1;
    }
    printf("info=wds bound to pcie iface %u mux %u\n", ep_iface, mux_id);
    return 0;
}

static int wds_start(const char *apn, uint8_t fam, uint32_t *handle) {
    struct msg m;
    struct resp r;
    uint16_t txn;
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_START_NETWORK, &txn);
    if (apn && strlen(apn) > 100) {
        printf("error=apn too long\n");
        return -1;
    }
    if (apn && *apn) tlv(&m, 0x14, apn, strlen(apn));
    tlv8(&m, 0x19, fam);
    s_abortable = 1;
    int failed = transact(&m, SVC_WDS, WDS_START_NETWORK, txn, &r, 90000);
    s_abortable = 0;
    if (failed) {
        uint16_t l;
        const uint8_t *v;
        if ((v = find_tlv(&r, 0x10, &l)) && l >= 2) printf("end_reason=%u\n", get16(v));
        if ((v = find_tlv(&r, 0x11, &l)) && l >= 4)
            printf("end_reason=verbose type %u reason %u\n", get16(v), get16(v + 2));
        /* 26 = NoEffect: a call on this APN is already up; reuse it */
        if (r.error == 26) {
            printf("warning=call already up (NoEffect)\n");
            *handle = 0;
            return 0;
        }
        printf("error=start network: qmi error %u\n", r.error);
        return -1;
    }
    uint16_t l;
    const uint8_t *v = find_tlv(&r, 0x01, &l);
    *handle = (v && l >= 4) ? get32(v) : 0;
    return 0;
}

static void wds_stop(uint32_t handle) {
    struct msg m;
    struct resp r;
    uint16_t txn;

    /* handle 0 means "NoEffect": the bearer already existed and this client did
     * not start it. Never tear down a call we do not own. */
    if (!handle) {
        printf("info=skip stop: bearer was not started by this client\n");
        return;
    }
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_STOP_NETWORK, &txn);
    tlv32(&m, 0x01, handle);
    transact(&m, SVC_WDS, WDS_STOP_NETWORK, txn, &r, 5000);
}

/* ---------- netdev ---------- */

static int find_ifname(char *out, size_t len) {
    const char *env = getenv("KEBAB_DATA_IFACE");
    if (env && *env) {
        snprintf(out, len, "%s", env);
        return 0;
    }
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (!strncmp(e->d_name, "mhi_hwip", 8)) {
            snprintf(out, len, "%.15s", e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static void iface_setup(const char *ifname, int up, uint32_t mtu) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (mtu) {
        ifr.ifr_mtu = mtu;
        if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) printf("warning=set mtu: %s\n", strerror(errno));
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        if (up) ifr.ifr_flags |= IFF_UP;
        else ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            printf("warning=set %s %s: %s\n", ifname, up ? "up" : "down", strerror(errno));
    }
    close(fd);
}

/* ---------- rmnet link creation (QMAP probe mode) ---------- */

#define NLMSG_TAIL(nmsg) \
    ((struct rtattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static void nl_attr(struct nlmsghdr *n, size_t maxlen, int type,
                    const void *data, size_t len) {
    size_t alen = RTA_LENGTH(len);
    struct rtattr *rta;

    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(alen) > maxlen) return;
    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = alen;
    if (len) memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(alen);
}

static void nl_attr_u16(struct nlmsghdr *n, size_t maxlen, int type, uint16_t v) {
    nl_attr(n, maxlen, type, &v, sizeof(v));
}

static void nl_attr_u32(struct nlmsghdr *n, size_t maxlen, int type, uint32_t v) {
    nl_attr(n, maxlen, type, &v, sizeof(v));
}

static void nl_attr_str(struct nlmsghdr *n, size_t maxlen, int type, const char *s) {
    nl_attr(n, maxlen, type, s, strlen(s) + 1);
}

static int nl_close_attr(struct nlmsghdr *n, struct rtattr *rta) {
    rta->rta_len = (char *)NLMSG_TAIL(n) - (char *)rta;
    return 0;
}

/* RTM_NEWLINK: rmnet_data<mux> on <base>, QMAP mux <mux>. */
static int rmnet_link_add(const char *base, const char *name, unsigned int mux) {
    struct {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char buf[512];
    } req;
    struct sockaddr_nl sa;
    struct rtattr *linkinfo, *data;
    unsigned int base_idx = if_nametoindex(base);
    char resp[1024];
    ssize_t n;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    req.n.nlmsg_seq = 1;
    req.i.ifi_family = AF_UNSPEC;

    if (!base_idx) {
        printf("error=no base netdev %s\n", base);
        return -ENODEV;
    }

    nl_attr_str(&req.n, sizeof(req), IFLA_IFNAME, name);
    nl_attr_u32(&req.n, sizeof(req), IFLA_LINK, base_idx);

    linkinfo = NLMSG_TAIL(&req.n);
    nl_attr(&req.n, sizeof(req), IFLA_LINKINFO, NULL, 0);
    nl_attr_str(&req.n, sizeof(req), IFLA_INFO_KIND, "rmnet");

    data = NLMSG_TAIL(&req.n);
    nl_attr(&req.n, sizeof(req), IFLA_INFO_DATA, NULL, 0);
    nl_attr_u16(&req.n, sizeof(req), IFLA_RMNET_MUX_ID, (uint16_t)mux);
    nl_close_attr(&req.n, data);
    nl_close_attr(&req.n, linkinfo);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -errno;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.n.nlmsg_len, 0, (struct sockaddr *)&sa,
               sizeof(sa)) < 0) {
        int err = -errno;
        close(fd);
        return err;
    }

    n = recv(fd, resp, sizeof(resp), 0);
    close(fd);
    if (n < (ssize_t)sizeof(struct nlmsghdr)) return -EIO;
    struct nlmsghdr *nh = (struct nlmsghdr *)resp;
    if (nh->nlmsg_type == NLMSG_ERROR && n >= (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(nh);
        return e->error; /* 0 = success, negative errno otherwise */
    }
    return -EIO;
}

static int qmap_prepare_link(void) {
    const char *base = getenv("KEBAB_QMAP_DEV");
    char name[IFNAMSIZ];
    int err;

    if (!base || !*base) base = "mhi_hwip0";
    snprintf(name, sizeof(name), "rmnet_data%u", s_qmap_mux);
    err = rmnet_link_add(base, name, s_qmap_mux);
    if (err && err != -EEXIST) {
        printf("error=create %s on %s mux %u: %s\n", name, base,
               s_qmap_mux, strerror(-err));
        return -1;
    }
    iface_setup(base, 1, 0);
    iface_setup(name, 1, 0);
    setenv("KEBAB_DATA_IFACE", name, 1);
    printf("info=qmap mux %u on %s/%s\n", s_qmap_mux, base, name);
    return 0;
}

/*
 * Put the session address on the netdev. On stock Qualcomm netmgrd does this
 * and the Cuttlefish RIL only does it on its AT path; netd/ConnectivityService
 * never add addresses for a cellular agent, so without this the link has
 * routes but no source address and validation fails.
 */
static uint32_t s_v4_addr;       /* host order, 0 = none assigned */
static uint8_t s_v6_addr[16];
static int s_v6_prefix = -1;

static void iface_set_v4(const char *ifname, uint32_t addr, int prefix) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0)
        printf("warning=set address on %s: %s\n", ifname, strerror(errno));
    else if (addr) {
        sin->sin_addr.s_addr = htonl(prefix ? 0xffffffffu << (32 - prefix) : 0);
        if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
            printf("warning=set netmask on %s: %s\n", ifname, strerror(errno));
    }
    close(fd);
}

static void iface_v6(const char *ifname, const uint8_t *addr, int prefix, int add) {
    int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) == 0) {
        struct in6_ifreq r6;
        memset(&r6, 0, sizeof(r6));
        memcpy(&r6.ifr6_addr, addr, 16);
        r6.ifr6_prefixlen = prefix;
        r6.ifr6_ifindex = ifr.ifr_ifindex;
        if (ioctl(fd, add ? SIOCSIFADDR : SIOCDIFADDR, &r6) < 0 && add)
            printf("warning=set ipv6 address on %s: %s\n", ifname, strerror(errno));
    }
    close(fd);
}

/* SIOCSIFADDR with 0.0.0.0 does not remove an address; SIOCDIFADDR does. */
static void iface_del_v4(const char *ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = s_v4_addr ? htonl(s_v4_addr) : 0;
    if (ioctl(fd, SIOCDIFADDR, &ifr) < 0)
        printf("warning=clear ipv4 address on %s: %s\n", ifname, strerror(errno));
    close(fd);
}

static void iface_clear_addrs(const char *ifname) {
    if (s_v4_addr) iface_del_v4(ifname);
    if (s_v6_prefix >= 0) iface_v6(ifname, s_v6_addr, s_v6_prefix, 0);
    s_v4_addr = 0;
    s_v6_prefix = -1;
}

/* QMI carries IPv4 addresses as a little-endian u32 whose MSB is the first octet */
static void print_v4(const char *key, uint32_t v, int prefix) {
    char s[INET_ADDRSTRLEN];
    struct in_addr in = {.s_addr = htonl(v)};
    inet_ntop(AF_INET, &in, s, sizeof(s));
    if (prefix >= 0) printf("%s=%s/%d\n", key, s, prefix);
    else printf("%s=%s\n", key, s);
}

static void print_v6(const char *key, const uint8_t *a, int prefix) {
    char s[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, a, s, sizeof(s));
    if (prefix >= 0) printf("%s=%s/%d\n", key, s, prefix);
    else printf("%s=%s\n", key, s);
}

/*
 * Liveness check while the call is up. The X55 can drop or re-establish the
 * bearer on a RAT change (NR <-> LTE) without sending this client a
 * packet-service-status indication: TX keeps counting, RX stops, and Android
 * still shows the network as connected. Ask the modem directly instead.
 * Returns 1 if the call is gone (or its address changed), 0 if it is fine.
 */
static int wds_call_lost(uint8_t fam) {
    struct msg m;
    struct resp r;
    uint16_t txn, l;
    const uint8_t *v;

    msg_init(&m, SVC_WDS, s_wds_cid, WDS_PKT_SRVC_STATUS, &txn);
    if (transact(&m, SVC_WDS, WDS_PKT_SRVC_STATUS, txn, &r, 5000))
        return 0; /* no answer is not proof of loss; the next poll retries */
    if ((v = find_tlv(&r, 0x01, &l)) && l >= 1 && v[0] != 2 /* connected */) {
        printf("watchdog=status %u\n", v[0]);
        return 1;
    }
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_GET_CURRENT_SETTINGS, &txn);
    tlv32(&m, 0x10, (1 << 8)); /* IP address */
    if (transact(&m, SVC_WDS, WDS_GET_CURRENT_SETTINGS, txn, &r, 5000)) return 0;
    if (fam == 4 && s_v4_addr && (v = find_tlv(&r, 0x1e, &l)) && l >= 4 &&
        get32(v) != s_v4_addr) {
        printf("watchdog=ipv4 address changed\n");
        return 1;
    }
    if (fam == 6 && (v = find_tlv(&r, 0x25, &l)) && l >= 17 && memcmp(v, s_v6_addr, 16)) {
        printf("watchdog=ipv6 address changed\n");
        return 1;
    }
    return 0;
}

static int report_settings(const char *ifname, uint8_t fam) {
    struct msg m;
    struct resp r;
    uint16_t txn, l;
    const uint8_t *v;
    int have_addr = 0;
    uint32_t mtu = 0;

    msg_init(&m, SVC_WDS, s_wds_cid, WDS_GET_CURRENT_SETTINGS, &txn);
    /* DNS | IP address | gateway | MTU | IP family */
    tlv32(&m, 0x10, (1 << 4) | (1 << 8) | (1 << 9) | (1 << 13) | (1 << 15));
    if (transact(&m, SVC_WDS, WDS_GET_CURRENT_SETTINGS, txn, &r, 10000)) {
        printf("error=get current settings: qmi error %u\n", r.error);
        return -1;
    }

    printf("ifname=%s\n", ifname);
    if (fam == 4) {
        uint32_t mask = 0xffffffff;
        if ((v = find_tlv(&r, 0x21, &l)) && l >= 4) mask = get32(v);
        if ((v = find_tlv(&r, 0x1e, &l)) && l >= 4) {
            print_v4("address", get32(v), __builtin_popcount(mask));
            iface_set_v4(ifname, get32(v), __builtin_popcount(mask));
            s_v4_addr = get32(v);
            have_addr = 1;
        }
        if ((v = find_tlv(&r, 0x20, &l)) && l >= 4) print_v4("gateway", get32(v), -1);
        if ((v = find_tlv(&r, 0x15, &l)) && l >= 4) print_v4("dns", get32(v), -1);
        if ((v = find_tlv(&r, 0x16, &l)) && l >= 4) print_v4("dns", get32(v), -1);
    } else {
        if ((v = find_tlv(&r, 0x25, &l)) && l >= 17) {
            print_v6("address", v, v[16]);
            iface_v6(ifname, v, v[16], 1);
            memcpy(s_v6_addr, v, 16);
            s_v6_prefix = v[16];
            have_addr = 1;
        }
        if ((v = find_tlv(&r, 0x26, &l)) && l >= 16) print_v6("gateway", v, -1);
        if ((v = find_tlv(&r, 0x27, &l)) && l >= 16) print_v6("dns", v, -1);
        if ((v = find_tlv(&r, 0x28, &l)) && l >= 16) print_v6("dns", v, -1);
    }
    if ((v = find_tlv(&r, 0x29, &l)) && l >= 4) {
        mtu = get32(v);
        printf("mtu=%u\n", mtu);
    }
    if (!have_addr) {
        printf("error=modem reported no address\n");
        return -1;
    }
    iface_setup(ifname, 1, mtu);
    return 0;
}

static void on_signal(int sig) {
    (void)sig;
    s_stop = 1;
}

/* ---------- diagnostics: kebab-dataconn --probe [--set-mode N] ---------- */

static const char *opmode_name(uint8_t m) {
    static const char *names[] = {"online", "low-power", "factory-test", "offline",
                                  "resetting", "shutting-down", "persistent-low-power",
                                  "mode-only-low-power"};
    return m < sizeof(names) / sizeof(names[0]) ? names[m] : "unknown";
}

static void dms_simple(uint8_t cid, uint16_t msgid, struct resp *r, int *ok) {
    struct msg m;
    uint16_t txn;
    msg_init(&m, SVC_DMS, cid, msgid, &txn);
    *ok = transact(&m, SVC_DMS, msgid, txn, r, 5000) == 0;
    if (!*ok) printf("dms msg 0x%04x: result %u error %u\n", msgid, r->result, r->error);
}

static int probe(int set_mode) {
    struct resp r;
    uint16_t l;
    const uint8_t *v;
    uint8_t cid;
    int ok;

    if (ctl_get_cid(SVC_DMS, &cid)) {
        printf("error=allocate DMS client\n");
        return 1;
    }

    dms_simple(cid, DMS_GET_REVISION, &r, &ok);
    if (ok && (v = find_tlv(&r, 0x01, &l))) printf("revision=%.*s\n", l, v);

    dms_simple(cid, DMS_GET_CAPABILITIES, &r, &ok);
    if (ok && (v = find_tlv(&r, 0x01, &l)) && l >= 11)
        printf("capabilities: max_tx %u max_rx %u data_service %u sim %u radio_ifaces %u\n",
               get32(v), get32(v + 4), v[8], v[9], v[10]);
    if (ok && (v = find_tlv(&r, 0x10, &l)) && l >= 8)
        printf("service_capability_ext: %u\n", get32(v));

    dms_simple(cid, DMS_GET_OPERATING_MODE, &r, &ok);
    if (ok && (v = find_tlv(&r, 0x01, &l)) && l >= 1)
        printf("operating_mode=%u (%s)\n", v[0], opmode_name(v[0]));
    if (ok && (v = find_tlv(&r, 0x10, &l)) && l >= 2) {
        uint16_t why = get16(v);
        printf("offline_reason=0x%04x%s%s%s%s\n", why,
               (why & 1) ? " host-image-misconfiguration" : "",
               (why & 2) ? " pri-image-misconfiguration" : "",
               (why & 4) ? " pri-version-incompatible" : "",
               (why & 8) ? " device-memory-full" : "");
    }
    if (ok && (v = find_tlv(&r, 0x11, &l)) && l >= 1)
        printf("hardware_restricted=%u\n", v[0]);
    if (ok) {
        /* dump all TLVs so nothing firmware-specific is missed */
        size_t off = 0;
        while (off + 3 <= r.tlvs_len) {
            uint16_t tl = get16(r.tlvs + off + 1);
            printf("  opmode tlv 0x%02x len %u:", r.tlvs[off], tl);
            for (uint16_t i = 0; i < tl && off + 3 + i < r.tlvs_len && i < 32; i++)
                printf(" %02x", r.tlvs[off + 3 + i]);
            printf("\n");
            off += 3 + tl;
        }
    }

    if (set_mode >= 0) {
        struct msg m;
        uint16_t txn;
        msg_init(&m, SVC_DMS, cid, DMS_SET_OPERATING_MODE, &txn);
        tlv8(&m, 0x01, (uint8_t)set_mode);
        int ret = transact(&m, SVC_DMS, DMS_SET_OPERATING_MODE, txn, &r, 15000);
        printf("set_operating_mode %d (%s): %s (result %u error %u)\n", set_mode,
               opmode_name(set_mode), ret ? "FAILED" : "ok", r.result, r.error);
    }

    ctl_release_cid(SVC_DMS, cid);
    return 0;
}

/* ---------- diagnostics over QRTR: kebab-dataconn --qrtr [--set-mode N] ---------- */

static const char *qmi_svc_name(uint32_t svc) {
    switch (svc) {
    case 0x01: return "WDS";
    case 0x02: return "DMS";
    case 0x03: return "NAS";
    case 0x04: return "QOS";
    case 0x05: return "WMS (SMS)";
    case 0x09: return "VOICE";
    case 0x0b: return "UIM";
    case 0x0c: return "PBM";
    case 0x10: return "LOC (GNSS)";
    case 0x11: return "SAR";
    case 0x12: return "IMS settings";
    case 0x14: return "RMTFS";
    case 0x18: return "TIME";
    case 0x1a: return "WDA";
    case 0x1e: return "THERMAL/TMD";
    case 0x22: return "COEX";
    case 0x24: return "PDC (MBN config)";
    case 0x29: return "SSCTL";
    case 0x2b: return "SSCTL v2";
    case 0x2f: return "DPM";
    case 0x31: return "SLIMBUS/SSREQ";
    case 0x40: return "PDR / servreg-notif";
    case 0x41: return "SERVREG locator";
    case 0x42: return "SERVREG notifier";
    case 0x45: return "WLFW";
    default: return "";
    }
}

struct qsvc { uint32_t svc, ver, inst, node, port; };

static int qrtr_lookup(int fd, uint32_t local_node, struct qsvc *out, int max) {
    struct qrtr_ctrl_pkt pkt;
    struct sockaddr_qrtr sq = {.sq_family = AF_QIPCRTR, .sq_node = local_node,
                               .sq_port = QRTR_PORT_CTRL};
    int n = 0;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    if (sendto(fd, &pkt, sizeof(pkt), 0, (void *)&sq, sizeof(sq)) < 0) {
        printf("error=qrtr lookup send: %s\n", strerror(errno));
        return -1;
    }
    long deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 500) <= 0) continue;
        ssize_t len = recv(fd, &pkt, sizeof(pkt), 0);
        if (len < (ssize_t)sizeof(pkt) || pkt.cmd != QRTR_TYPE_NEW_SERVER) continue;
        if (!pkt.server.service && !pkt.server.instance && !pkt.server.node &&
            !pkt.server.port)
            break; /* end of the lookup listing */
        if (n < max) {
            out[n].svc = pkt.server.service;
            out[n].ver = pkt.server.instance & 0xff;
            out[n].inst = pkt.server.instance >> 8;
            out[n].node = pkt.server.node;
            out[n].port = pkt.server.port;
            n++;
        }
    }
    return n;
}

/* One QMI request/response over QRTR. The SDU is the QMUX message minus its 6-byte header. */
static int qrtr_transact(int fd, const struct qsvc *q, struct msg *m, uint16_t msgid,
                         uint16_t txn, struct resp *r) {
    uint16_t tl = m->len - m->tlv_off - 2;
    m->buf[m->tlv_off] = tl & 0xff;
    m->buf[m->tlv_off + 1] = tl >> 8;
    struct sockaddr_qrtr sq = {.sq_family = AF_QIPCRTR, .sq_node = q->node, .sq_port = q->port};
    r->result = r->error = 0xffff;
    r->tlvs_len = 0;
    dump_frame("qmi tx", m->buf + 6, m->len - 6);
    if (sendto(fd, m->buf + 6, m->len - 6, 0, (void *)&sq, sizeof(sq)) < 0) {
        printf("error=qrtr send: %s\n", strerror(errno));
        return -1;
    }
    long deadline = now_ms() + 5000;
    while (now_ms() < deadline) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 500) <= 0) continue;
        ssize_t n = recv(fd, r->buf, sizeof(r->buf), 0);
        if (n < 7) continue;
        uint8_t flags = r->buf[0];
        uint16_t rtxn = get16(r->buf + 1), rmsg = get16(r->buf + 3), rl = get16(r->buf + 5);
        if (!(flags & 0x02) || rtxn != txn || rmsg != msgid) continue;
        dump_frame("qmi rx", r->buf, n);
        r->len = n;
        r->tlvs = r->buf + 7;
        r->tlvs_len = (size_t)n - 7 < rl ? (size_t)n - 7 : rl;
        uint16_t l;
        const uint8_t *res = find_tlv(r, 0x02, &l);
        if (res && l >= 4) {
            r->result = get16(res);
            r->error = get16(res + 2);
        }
        return r->result == 0 ? 0 : -1;
    }
    printf("warning=qrtr timeout svc 0x%02x msg 0x%04x\n", q->svc, msgid);
    return -1;
}

static void dump_tlvs(const char *what, const struct resp *r) {
    size_t off = 0;
    while (off + 3 <= r->tlvs_len) {
        uint16_t tl = get16(r->tlvs + off + 1);
        printf("  %s tlv 0x%02x len %u:", what, r->tlvs[off], tl);
        for (uint16_t i = 0; i < tl && off + 3 + i < r->tlvs_len && i < 128; i++)
            printf(" %02x", r->tlvs[off + 3 + i]);
        printf("\n");
        off += 3 + tl;
    }
}

static int qrtr_probe(int set_mode) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (getsockname(fd, (void *)&me, &sl) < 0) {
        printf("error=getsockname: %s\n", strerror(errno));
        return 1;
    }
    printf("local node %u port %u\n", me.sq_node, me.sq_port);

    struct qsvc list[128];
    int n = qrtr_lookup(fd, me.sq_node, list, 128);
    printf("services: %d\n", n);
    const struct qsvc *dms = NULL;
    for (int i = 0; i < n; i++) {
        printf("  svc 0x%02x v%u inst %u  node %u port %u  %s\n", list[i].svc, list[i].ver,
               list[i].inst, list[i].node, list[i].port, qmi_svc_name(list[i].svc));
        if (list[i].svc == SVC_DMS && list[i].node != me.sq_node && !dms) dms = &list[i];
    }
    if (!dms) {
        printf("error=no remote DMS service\n");
        close(fd);
        return 1;
    }

    struct msg m;
    struct resp r;
    uint16_t txn;
    const uint8_t *v;
    uint16_t l;

    msg_init(&m, SVC_DMS, 0, DMS_GET_REVISION, &txn);
    if (!qrtr_transact(fd, dms, &m, DMS_GET_REVISION, txn, &r) && (v = find_tlv(&r, 0x01, &l)))
        printf("revision=%.*s\n", l, v);

    msg_init(&m, SVC_DMS, 0, DMS_GET_OPERATING_MODE, &txn);
    int ret = qrtr_transact(fd, dms, &m, DMS_GET_OPERATING_MODE, txn, &r);
    printf("get_operating_mode: result %u error %u\n", r.result, r.error);
    if (!ret) {
        if ((v = find_tlv(&r, 0x01, &l)) && l >= 1)
            printf("operating_mode=%u (%s)\n", v[0], opmode_name(v[0]));
        if ((v = find_tlv(&r, 0x10, &l)) && l >= 2) {
            uint16_t why = get16(v);
            printf("offline_reason=0x%04x%s%s%s%s\n", why,
                   (why & 1) ? " host-image-misconfiguration" : "",
                   (why & 2) ? " pri-image-misconfiguration" : "",
                   (why & 4) ? " pri-version-incompatible" : "",
                   (why & 8) ? " device-memory-full" : "");
        }
        if ((v = find_tlv(&r, 0x11, &l)) && l >= 1) printf("hardware_restricted=%u\n", v[0]);
        dump_tlvs("opmode", &r);
    }

    if (set_mode >= 0) {
        msg_init(&m, SVC_DMS, 0, DMS_SET_OPERATING_MODE, &txn);
        tlv8(&m, 0x01, (uint8_t)set_mode);
        ret = qrtr_transact(fd, dms, &m, DMS_SET_OPERATING_MODE, txn, &r);
        printf("set_operating_mode %d (%s): %s (result %u error %u)\n", set_mode,
               opmode_name(set_mode), ret ? "FAILED" : "ok", r.result, r.error);
    }
    close(fd);
    return 0;
}

/*
 * kebab-dataconn --qmi <svc> <msgid> [type:hexbytes ...]
 * e.g. --qmi 0x02 0x0045            (DMS get band capability)
 *      --qmi 0x02 0x002e 01:01      (DMS set operating mode low-power)
 */
static int qrtr_raw(int argc, char **argv) {
    uint32_t svc = strtoul(argv[2], NULL, 0);
    uint16_t msgid = strtoul(argv[3], NULL, 0);
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (fd < 0 || getsockname(fd, (void *)&me, &sl) < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        return 1;
    }
    struct qsvc list[128];
    int n = qrtr_lookup(fd, me.sq_node, list, 128);
    const struct qsvc *q = NULL;
    for (int i = 0; i < n; i++)
        if (list[i].svc == svc && list[i].node != me.sq_node) { q = &list[i]; break; }
    if (!q) {
        printf("error=service 0x%x not found\n", svc);
        return 1;
    }
    /* several requests on one client: --qmi svc msg [tlv...] + msg2 [tlv...] + ... */
    int ret = 0;
    for (int i = 3; i < argc;) {
        struct msg m;
        struct resp r;
        uint16_t txn;
        msgid = strtoul(argv[i++], NULL, 0);
        msg_init(&m, (uint8_t)svc, 0, msgid, &txn);
        for (; i < argc && strcmp(argv[i], "+"); i++) {
            uint8_t type = strtoul(argv[i], NULL, 16), data[256];
            const char *h = strchr(argv[i], ':');
            uint16_t len = 0;
            if (h) {
                for (h++; h[0] && h[1] && len < sizeof(data); h += 2) {
                    char b[3] = {h[0], h[1], 0};
                    data[len++] = strtoul(b, NULL, 16);
                }
            }
            tlv(&m, type, data, len);
        }
        if (i < argc) i++; /* skip "+" */
        ret = qrtr_transact(fd, q, &m, msgid, txn, &r);
        printf("svc 0x%02x msg 0x%04x: %s result %u error %u\n", svc, msgid,
               ret ? "FAILED" : "ok", r.result, r.error);
        dump_tlvs("resp", &r);
    }
    /* KEBAB_QMI_LISTEN=<secs>: then print indications from this client (e.g. WMS MT SMS) */
    const char *ls = getenv("KEBAB_QMI_LISTEN");
    long until = now_ms() + (ls ? atol(ls) * 1000 : 0);
    while (now_ms() < until) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        struct resp r;
        if (poll(&pfd, 1, 500) <= 0) continue;
        ssize_t n = recv(fd, r.buf, sizeof(r.buf), 0);
        if (n < 7) continue;
        r.tlvs = r.buf + 7;
        r.tlvs_len = (size_t)n - 7;
        printf("ind flags 0x%02x msg 0x%04x\n", r.buf[0], get16(r.buf + 3));
        dump_tlvs("ind", &r);
        fflush(stdout);
    }
    close(fd);
    return ret ? 1 : 0;
}

/* ---------- DIAG (QCDM) NV reads: kebab-dataconn --diag-nv <item> [item ...] ---------- */

static uint16_t crc16_ccitt(const uint8_t *p, size_t n) {
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return ~crc;
}

static int diag_xfer(int fd, const uint8_t *req, size_t len, uint8_t *out, size_t outmax) {
    uint8_t frame[1024], raw[1024];
    size_t n = 0;
    uint16_t crc = crc16_ccitt(req, len);
    uint8_t tmp[600];
    memcpy(tmp, req, len);
    tmp[len] = crc & 0xff;
    tmp[len + 1] = crc >> 8;
    for (size_t i = 0; i < len + 2; i++) {
        if (tmp[i] == 0x7e || tmp[i] == 0x7d) {
            frame[n++] = 0x7d;
            frame[n++] = tmp[i] ^ 0x20;
        } else {
            frame[n++] = tmp[i];
        }
    }
    frame[n++] = 0x7e;
    if (write(fd, frame, n) != (ssize_t)n) return -1;

    /* read frames until one answers our command code (skip async logs) */
    long deadline = now_ms() + 3000;
    size_t rn = 0;
    while (now_ms() < deadline) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 300) <= 0) continue;
        uint8_t b[2048];
        ssize_t got = read(fd, b, sizeof(b));
        for (ssize_t i = 0; i < got; i++) {
            if (b[i] == 0x7e) {
                /* unescape raw[0..rn) */
                size_t m = 0;
                for (size_t j = 0; j < rn && m < outmax; j++)
                    out[m++] = (raw[j] == 0x7d && j + 1 < rn) ? (raw[++j] ^ 0x20) : raw[j];
                rn = 0;
                if (m >= 3 && out[0] == req[0]) return (int)m - 2;
                if (m >= 3) printf("  (diag frame cmd 0x%02x len %zu skipped)\n", out[0], m);
                continue;
            }
            if (rn < sizeof(raw)) raw[rn++] = b[i];
        }
    }
    return -1;
}

static int diag_nv(int argc, char **argv) {
    const char *dev = getenv("KEBAB_DIAG_DEV");
    int fd = open(dev && *dev ? dev : "/dev/wwan0qcdm0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("error=open diag: %s\n", strerror(errno));
        return 1;
    }
    static const char *st[] = {"done", "busy", "bad-cmd", "full", "fail", "not-active",
                               "bad-param", "read-only", "bad-tbl", "no-mem"};
    for (int a = 2; a < argc; a++) {
        uint16_t item = strtoul(argv[a], NULL, 0);
        uint8_t req[1 + 2 + 128 + 2] = {0x26, item & 0xff, item >> 8};
        uint8_t resp[1024];
        int n = diag_xfer(fd, req, sizeof(req), resp, sizeof(resp));
        if (n < (int)sizeof(req)) {
            printf("nv %u: no/short response (%d)%s\n", item, n,
                   n > 0 && resp[0] == 0x13 ? " [bad command]" : "");
            continue;
        }
        uint16_t status = get16(resp + 131);
        printf("nv %u: status %u (%s) data:", item, status, status < 10 ? st[status] : "?");
        int last = 130;
        while (last > 3 && resp[last] == 0) last--;
        for (int i = 3; i <= last && i < 3 + 64; i++) printf(" %02x", resp[i]);
        printf("\n");
    }
    close(fd);
    return 0;
}

/*
 * kebab-dataconn --diag-msgs <seconds> [grep]
 * Enables every F3 debug message (DIAG ext msg config, set all RT masks),
 * prints the plain-text ones (cmd 0x79) and counts hashed QShrink ones,
 * then turns the masks back off.
 */
static void diag_send_raw(int fd, const uint8_t *req, size_t len) {
    uint8_t frame[256], tmp[128];
    size_t n = 0;
    uint16_t crc = crc16_ccitt(req, len);
    memcpy(tmp, req, len);
    tmp[len] = crc & 0xff;
    tmp[len + 1] = crc >> 8;
    for (size_t i = 0; i < len + 2; i++) {
        if (tmp[i] == 0x7e || tmp[i] == 0x7d) {
            frame[n++] = 0x7d;
            frame[n++] = tmp[i] ^ 0x20;
        } else {
            frame[n++] = tmp[i];
        }
    }
    frame[n++] = 0x7e;
    if (write(fd, frame, n) != (ssize_t)n) printf("warning=diag write failed\n");
}

static void diag_print_msg(const uint8_t *f, size_t m, const char *filter, unsigned *hashed) {
    if (f[0] == 0x99 || f[0] == 0x92 || f[0] == 0x9a) {
        /* QShrink: dump raw, decoded on the host against qdsp6m.qdb */
        (*hashed)++;
        printf("H");
        for (size_t i = 0; i < m && i < 160; i++) printf(" %02x", f[i]);
        putchar('\n');
        return;
    }
    if (f[0] != 0x79 || m < 20) return;
    uint8_t nargs = f[2];
    uint16_t line = get16(f + 12), ssid = get16(f + 14);
    size_t off = 20 + 4 * (size_t)nargs;
    if (off >= m) return;
    const char *fmt = (const char *)f + off;
    size_t fl = strnlen(fmt, m - off);
    const char *file = fl + 1 < m - off ? fmt + fl + 1 : "";
    if (filter && !strcasestr(fmt, filter) && !strcasestr(file, filter)) return;
    printf("[ssid %u] %.*s:%u: ", ssid, (int)strnlen(file, m - off - fl - 1), file, line);
    /* print the format with up to 4 numeric args substituted crudely */
    uint32_t args[16];
    for (int i = 0; i < nargs && i < 16; i++) args[i] = get32(f + 20 + 4 * i);
    int ai = 0;
    for (size_t i = 0; i < fl; i++) {
        if (fmt[i] == '%' && i + 1 < fl) {
            size_t j = i + 1;
            while (j < fl && strchr("0123456789.-+#lhz", fmt[j])) j++;
            if (j < fl && fmt[j] != '%') {
                if (fmt[j] == 's') printf("<str>");
                else if (ai < nargs && ai < 16) printf(fmt[j] == 'x' || fmt[j] == 'X' ? "0x%x" : "%d", args[ai]);
                ai++;
                i = j;
                continue;
            }
        }
        putchar(fmt[i] == '\n' ? ' ' : fmt[i]);
    }
    putchar('\n');
}

static int diag_msgs(int argc, char **argv) {
    int secs = atoi(argv[2]);
    const char *filter = argc > 3 ? argv[3] : NULL;
    const char *dev = getenv("KEBAB_DIAG_DEV");
    int fd = open(dev && *dev ? dev : "/dev/wwan0qcdm0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("error=open diag: %s\n", strerror(errno));
        return 1;
    }
    /* DIAG_EXT_MSG_CONFIG_F / SET_ALL_RT_MASKS: enable everything */
    uint8_t on[] = {0x7d, 0x05, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff};
    diag_send_raw(fd, on, sizeof(on));

    uint8_t raw[8192], f[8192];
    size_t rn = 0;
    unsigned frames = 0, hashed = 0;
    long end = now_ms() + secs * 1000L;
    while (now_ms() < end && !s_stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 300) <= 0) continue;
        uint8_t b[8192];
        ssize_t got = read(fd, b, sizeof(b));
        for (ssize_t i = 0; i < got; i++) {
            if (b[i] != 0x7e) {
                if (rn < sizeof(raw)) raw[rn++] = b[i];
                continue;
            }
            size_t m = 0;
            for (size_t j = 0; j < rn && m < sizeof(f); j++)
                f[m++] = (raw[j] == 0x7d && j + 1 < rn) ? (raw[++j] ^ 0x20) : raw[j];
            rn = 0;
            if (m < 3) continue;
            frames++;
            diag_print_msg(f, m - 2, filter, &hashed);
        }
    }
    uint8_t off[] = {0x7d, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    diag_send_raw(fd, off, sizeof(off));
    printf("frames %u, hashed (QShrink, dumped as H lines) %u\n", frames, hashed);
    close(fd);
    return 0;
}

/*
 * kebab-dataconn --diag-logs <seconds> <lo-hi>[,<lo-hi>...]
 * Enables DIAG log packets for the given log codes of equipment 1 (e.g. IMS:
 * 0x156e SIP message) and prints each one: "L <code> <len>" plus its text
 * (SIP is plain ASCII) or a hex dump. Masks are cleared again at the end.
 */
static void diag_send_big(int fd, const uint8_t *req, size_t len) {
    static uint8_t frame[2 * 1024 + 8];
    size_t n = 0;
    uint16_t crc = crc16_ccitt(req, len);
    uint8_t c[2] = {crc & 0xff, crc >> 8};
    for (size_t i = 0; i < len + 2 && n + 3 < sizeof(frame); i++) {
        uint8_t v = i < len ? req[i] : c[i - len];
        if (v == 0x7e || v == 0x7d) {
            frame[n++] = 0x7d;
            frame[n++] = v ^ 0x20;
        } else {
            frame[n++] = v;
        }
    }
    frame[n++] = 0x7e;
    if (write(fd, frame, n) != (ssize_t)n) printf("warning=diag write failed\n");
}

#define LOG_ITEMS 0x900 /* equipment 1 items 0x000..0x8ff = codes 0x1000..0x18ff */

static void diag_log_mask(int fd, const char *spec) {
    uint8_t req[16 + LOG_ITEMS / 8];
    memset(req, 0, sizeof(req));
    req[0] = 0x73;          /* DIAG_LOG_CONFIG_F */
    req[4] = 3;             /* SET_MASK */
    req[8] = 1;             /* equipment id 1 (codes 0x1xxx) */
    req[12] = LOG_ITEMS & 0xff;
    req[13] = LOG_ITEMS >> 8;
    for (const char *p = spec; p && *p;) {
        char *e;
        unsigned lo = strtoul(p, &e, 0), hi = lo;
        if (*e == '-') hi = strtoul(e + 1, &e, 0);
        for (unsigned c = lo; c <= hi; c++) {
            unsigned item = c - 0x1000;
            if (c >= 0x1000 && item < LOG_ITEMS) req[16 + item / 8] |= 1 << (item % 8);
        }
        p = *e == ',' ? e + 1 : NULL;
    }
    diag_send_big(fd, req, sizeof(req));
}

static int diag_logs(char **argv) {
    int secs = atoi(argv[2]);
    const char *dev = getenv("KEBAB_DIAG_DEV");
    int fd = open(dev && *dev ? dev : "/dev/wwan0qcdm0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("error=open diag: %s\n", strerror(errno));
        return 1;
    }
    diag_log_mask(fd, argv[3]);
    static uint8_t raw[65536], f[65536];
    size_t rn = 0;
    unsigned logs = 0;
    long end = now_ms() + secs * 1000L;
    while (now_ms() < end && !s_stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 300) <= 0) continue;
        uint8_t b[8192];
        ssize_t got = read(fd, b, sizeof(b));
        for (ssize_t i = 0; i < got; i++) {
            if (b[i] != 0x7e) {
                if (rn < sizeof(raw)) raw[rn++] = b[i];
                continue;
            }
            size_t m = 0;
            for (size_t j = 0; j < rn && m < sizeof(f); j++)
                f[m++] = (raw[j] == 0x7d && j + 1 < rn) ? (raw[++j] ^ 0x20) : raw[j];
            rn = 0;
            if (m < 18) continue;
            m -= 2; /* crc */
            if (f[0] == 0x73) {
                printf("log mask response: status %u\n", m >= 8 ? get32(f + 4) : 0xffff);
                continue;
            }
            if (f[0] != 0x10) continue;
            uint16_t code = get16(f + 6);
            logs++;
            printf("L 0x%04x %zu:", code, m - 16);
            size_t printable = 0;
            for (size_t k = 16; k < m; k++)
                if ((f[k] >= 0x20 && f[k] < 0x7f) || f[k] == '\r' || f[k] == '\n') printable++;
            for (size_t k = 16; k < m && k < 16 + 96; k++) printf(" %02x", f[k]);
            putchar('\n');
            if (m > 16 && printable * 2 > m - 16) {
                /* mostly text (SIP): print the readable part */
                for (size_t k = 16; k < m; k++) {
                    uint8_t ch = f[k];
                    if (ch == '\r') continue;
                    if (ch == '\n') printf("\n    ");
                    else putchar(ch >= 0x20 && ch < 0x7f ? ch : '.');
                }
                putchar('\n');
            }
        }
    }
    diag_log_mask(fd, "");
    printf("log packets %u\n", logs);
    close(fd);
    return 0;
}

/*
 * kebab-dataconn --qrtr-listen <svc> <ver> <inst> <seconds>
 * Publishes a QRTR server for one QMI service and dumps every request a
 * remote client sends to it (no answers). Used to see whether the modem's IMS
 * stack looks for an AP-side service (e.g. IMS DCM, 0x302).
 */
static int s_qrtr_ack; /* --qrtr-serve: answer every request with success */

static int qrtr_listen(char **argv) {
    uint32_t svc = strtoul(argv[2], NULL, 0), ver = strtoul(argv[3], NULL, 0);
    uint32_t inst = strtoul(argv[4], NULL, 0);
    int secs = atoi(argv[5]);
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (fd < 0 || getsockname(fd, (void *)&me, &sl) < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        return 1;
    }
    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_SERVER;
    pkt.server.service = svc;
    pkt.server.instance = (ver & 0xff) | (inst << 8);
    pkt.server.node = me.sq_node;
    pkt.server.port = me.sq_port;
    struct sockaddr_qrtr ctrl = {.sq_family = AF_QIPCRTR, .sq_node = me.sq_node,
                                 .sq_port = QRTR_PORT_CTRL};
    if (sendto(fd, &pkt, sizeof(pkt), 0, (void *)&ctrl, sizeof(ctrl)) < 0) {
        printf("error=publish: %s\n", strerror(errno));
        return 1;
    }
    printf("published svc 0x%x v%u inst %u at node %u port %u\n", svc, ver, inst, me.sq_node,
           me.sq_port);
    long end = now_ms() + secs * 1000L;
    unsigned n = 0;
    while (now_ms() < end && !s_stop) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 500) <= 0) continue;
        uint8_t b[4096];
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t len = recvfrom(fd, b, sizeof(b), 0, (void *)&from, &fl);
        if (len < 0) continue;
        n++;
        if (from.sq_port == QRTR_PORT_CTRL) {
            printf("ctrl from node %u: cmd %u\n", from.sq_node, len >= 4 ? get32(b) : 0);
            continue;
        }
        printf("from node %u port %u len %zd", from.sq_node, from.sq_port, len);
        if (len >= 7)
            printf(" type %u txn %u msg 0x%04x", b[0], get16(b + 1), get16(b + 3));
        printf(":");
        for (ssize_t i = 0; i < len && i < 128; i++) printf(" %02x", b[i]);
        printf("\n");
        if (s_qrtr_ack && len >= 7 && b[0] == 0x00) {
            /* plain QMI success response: result TLV 0x02 = {0, 0} */
            uint8_t r[14] = {0x02, b[1], b[2], b[3], b[4], 7, 0, 0x02, 4, 0, 0, 0, 0, 0};
            if (sendto(fd, r, sizeof(r), 0, (void *)&from, sizeof(from)) < 0)
                printf("warning=ack send: %s\n", strerror(errno));
        }
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_DEL_SERVER;
    pkt.server.service = svc;
    pkt.server.instance = (ver & 0xff) | (inst << 8);
    pkt.server.node = me.sq_node;
    pkt.server.port = me.sq_port;
    sendto(fd, &pkt, sizeof(pkt), 0, (void *)&ctrl, sizeof(ctrl));
    printf("packets %u\n", n);
    close(fd);
    return 0;
}

/*
 * ---------- IMS DCM server (QMI service 0x302) ----------
 *
 * Stand-in for the stock imsdatadaemon. Once the modem knows which APN is its
 * IMS one (QMI DSD SET_APN_INFO), its IMS client asks the AP to bring that PDN
 * up with PDP_ACTIVATE (0x20) and waits for PDP_ACTIVATE_IND with the call's
 * address. Layout from the stock IDL (imsdatadaemon, ims_dcm v01):
 *   req  0x20: 0x01 {u8 len + apn, u32 apn_type, u32 rat, u32 family (0 v4, 1 v6),
 *              u32 profile}, 0x10 u32 cookie, 0x11/0x12/0x13 u32
 *   resp 0x20: 0x02 result, 0x10 u8 pdp_id, 0x11 u32 cookie, 0x12 u32 (= req 0x13)
 *   ind  0x20: 0x02 result, 0x01 u8 pdp_id, 0x10 u32 cookie,
 *              0x11 {u32 family, u8 len + address string}, 0x12 u32
 *   req  0x21 (PDP_DEACTIVATE): 0x01 u8 pdp_id
 * Every other request (LINK_ADDR, REGISTER_APP_STATE, SERVICE_ENABLE, ...) is
 * answered with plain success, as the stock handlers do.
 *
 * The IMS call gets its own WDS client and is NOT bound to a data port: the
 * modem's IMS stack only needs the PDN up, and binding a second client to the
 * IP_HW0 endpoint once broke the default (jionet) data path.
 */
static int qrtr_open_clients(void);
#define DCM_SVC 0x302
#define DCM_PDP_ID 20

struct sdu {
    uint8_t b[1024];
    size_t len;
};

static void sdu_init(struct sdu *o, uint8_t type, uint16_t txn, uint16_t msgid) {
    o->b[0] = type;
    o->b[1] = txn & 0xff;
    o->b[2] = txn >> 8;
    o->b[3] = msgid & 0xff;
    o->b[4] = msgid >> 8;
    o->len = 7;
}

static void sdu_tlv(struct sdu *o, uint8_t t, const void *v, uint16_t l) {
    if (o->len + 3 + l > sizeof(o->b)) return;
    o->b[o->len++] = t;
    o->b[o->len++] = l & 0xff;
    o->b[o->len++] = l >> 8;
    memcpy(o->b + o->len, v, l);
    o->len += l;
}

static void sdu_tlv32(struct sdu *o, uint8_t t, uint32_t v) {
    uint8_t x[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
    sdu_tlv(o, t, x, 4);
}

static void sdu_result(struct sdu *o, uint16_t result, uint16_t error) {
    uint8_t x[4] = {result & 0xff, result >> 8, error & 0xff, error >> 8};
    sdu_tlv(o, 0x02, x, 4);
}

static int sdu_send(int fd, const struct sockaddr_qrtr *to, struct sdu *o) {
    uint16_t tl = o->len - 7;
    o->b[5] = tl & 0xff;
    o->b[6] = tl >> 8;
    return sendto(fd, o->b, o->len, 0, (const void *)to, sizeof(*to)) < 0 ? -1 : 0;
}

static const uint8_t *sdu_find(const uint8_t *b, size_t len, uint8_t t, uint16_t *l) {
    size_t end = 7 + (size_t)get16(b + 5);
    if (end > len) end = len;
    for (size_t off = 7; off + 3 <= end;) {
        uint16_t tl = get16(b + off + 1);
        if (off + 3 + tl > end) break;
        if (b[off] == t) {
            *l = tl;
            return b + off + 3;
        }
        off += 3 + tl;
    }
    return NULL;
}

/* Address of the call this client started, as text and raw; 0 on success */
static int wds_get_addr_full(uint8_t fam, char *out, size_t outlen, uint8_t *raw, int *prefix) {
    struct msg m;
    struct resp r;
    uint16_t txn, l;
    const uint8_t *v;
    msg_init(&m, SVC_WDS, s_wds_cid, WDS_GET_CURRENT_SETTINGS, &txn);
    tlv32(&m, 0x10, 0x000fffff); /* IP address, IP family, DNS, Gateway, P-CSCF */
    if (transact(&m, SVC_WDS, WDS_GET_CURRENT_SETTINGS, txn, &r, 10000)) return -1;
    dump_tlvs("wds-settings", &r);
    if (fam == 6 && (v = find_tlv(&r, 0x25, &l)) && l >= 17) {
        if (raw) memcpy(raw, v, 16);
        if (prefix) *prefix = v[16];
        return inet_ntop(AF_INET6, v, out, outlen) ? 0 : -1;
    }
    if (fam == 4 && (v = find_tlv(&r, 0x1e, &l)) && l >= 4) {
        struct in_addr a = {.s_addr = htonl(get32(v))};
        if (raw) memcpy(raw, v, 4);
        if (prefix) *prefix = 32;
        return inet_ntop(AF_INET, &a, out, outlen) ? 0 : -1;
    }
    return -1;
}

static int ims_dcm(int secs) {
    if (qrtr_open_clients() || s_qwds.fd < 0) {
        printf("error=no WDS service over QRTR\n");
        return 1;
    }
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (fd < 0 || getsockname(fd, (void *)&me, &sl) < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        return 1;
    }
    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_SERVER;
    pkt.server.service = DCM_SVC;
    pkt.server.instance = 1; /* version 1, instance 0 */
    pkt.server.node = me.sq_node;
    pkt.server.port = me.sq_port;
    struct sockaddr_qrtr ctrl = {.sq_family = AF_QIPCRTR, .sq_node = me.sq_node,
                                 .sq_port = QRTR_PORT_CTRL};
    if (sendto(fd, &pkt, sizeof(pkt), 0, (void *)&ctrl, sizeof(ctrl)) < 0) {
        printf("error=publish: %s\n", strerror(errno));
        return 1;
    }
    printf("info=ims dcm published at node %u port %u\n", me.sq_node, me.sq_port);

    uint32_t handle = 0;
    int call_up = 0;
    char ifname[IFNAMSIZ] = "mhi_hwip0";
    uint8_t ims_v6[16];
    int ims_v6_prefix = -1;
    find_ifname(ifname, sizeof(ifname));
    long end = secs > 0 ? now_ms() + secs * 1000L : 0;
    while (!s_stop && (!end || now_ms() < end)) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) continue;
        uint8_t b[2048];
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        ssize_t len = recvfrom(fd, b, sizeof(b), 0, (void *)&from, &fl);
        if (len < 7 || from.sq_port == QRTR_PORT_CTRL || b[0] != 0x00) continue;
        uint16_t txn = get16(b + 1), msgid = get16(b + 3), l;
        const uint8_t *v;
        struct sdu o;
        printf("info=dcm request 0x%04x from node %u port %u\n", msgid, from.sq_node,
               from.sq_port);

        if (msgid != 0x20 && msgid != 0x21) {
            sdu_init(&o, 0x02, txn, msgid);
            sdu_result(&o, 0, 0);
            sdu_send(fd, &from, &o);
            continue;
        }
        if (msgid == 0x21) {
            sdu_init(&o, 0x02, txn, msgid);
            sdu_result(&o, 0, 0);
            sdu_send(fd, &from, &o);
            if (call_up) {
                printf("info=ims pdp deactivate\n");
                if (ims_v6_prefix >= 0) {
                    iface_v6(ifname, ims_v6, ims_v6_prefix, 0);
                    ims_v6_prefix = -1;
                }
                wds_stop(handle);
                call_up = 0;
                handle = 0;
            }
            continue;
        }

        /* PDP_ACTIVATE */
        char apn[101] = "";
        uint32_t family = 1, cookie = 0, echo = 0;
        if ((v = sdu_find(b, len, 0x01, &l)) && l >= 1 && v[0] <= 100 && l >= 1 + v[0] + 16) {
            memcpy(apn, v + 1, v[0]);
            apn[v[0]] = 0;
            family = get32(v + 1 + v[0] + 8);
        }
        if ((v = sdu_find(b, len, 0x10, &l)) && l >= 4) cookie = get32(v);
        if ((v = sdu_find(b, len, 0x13, &l)) && l >= 4) echo = get32(v);
        uint8_t fam = family == 1 ? 6 : 4;
        printf("info=ims pdp activate apn \"%s\" ipv%u cookie %u\n", apn, fam, cookie);

        uint8_t pid = DCM_PDP_ID;
        sdu_init(&o, 0x02, txn, msgid);
        sdu_result(&o, 0, 0);
        sdu_tlv(&o, 0x10, &pid, 1);
        sdu_tlv32(&o, 0x11, cookie);
        sdu_tlv32(&o, 0x12, echo);
        sdu_send(fd, &from, &o);

        char addr[64] = "";
        uint8_t v6_raw[16];
        int v6_prefix = -1;
        int ok = call_up;
        if (!call_up) {
            /*
             * Over QRTR, Start Network on an unbound WDS client fails with
             * INVALID_OPERATION (70). KEBAB_IMS_BIND="<ep iface>:<mux>",
             * default 4:0 (same raw-IP pipe as the default call). mux 1 on
             * iface 4 once broke the default data path.
             */
            static int bound;
            if (!bound) {
                const char *bs = getenv("KEBAB_IMS_BIND");
                unsigned ep = 4, mux = s_qmap ? s_qmap_mux + 1 : 0;
                if (bs && *bs) sscanf(bs, "%u:%u", &ep, &mux);
                if (mux > 0xff) {
                    mux = s_qmap ? 2 : 0;
                    printf("warning=ignoring mux >255, using %u\n", mux);
                }
                bound = wds_bind_data_port(ep, mux) == 0;
            }
            wds_set_family(fam);
            ok = wds_start(*apn ? apn : "ims", fam, &handle) == 0;
            call_up = ok;
        }
        if (ok && wds_get_addr_full(fam, addr, sizeof(addr), v6_raw, &v6_prefix)) ok = 0;
        printf("info=ims pdp %s %s\n", ok ? "up" : "failed", addr);
        if (ok && fam == 6 && v6_prefix >= 0) {
            char rcmd[160];

            find_ifname(ifname, sizeof(ifname));
            iface_v6(ifname, v6_raw, v6_prefix, 1);
            memcpy(ims_v6, v6_raw, 16);
            ims_v6_prefix = v6_prefix;
            snprintf(rcmd, sizeof(rcmd),
                     "ip -6 route replace default dev %s table %s 2>/dev/null",
                     ifname, ifname);
            system(rcmd);
            system("ip -6 rule add pref 1000 lookup main 2>/dev/null");
        }

        sdu_init(&o, 0x04, 0, 0x20);
        sdu_result(&o, ok ? 0 : 1, ok ? 0 : 14 /* CALL_FAILED */);
        sdu_tlv(&o, 0x01, &pid, 1);
        sdu_tlv32(&o, 0x10, cookie);
        if (ok) {
            uint8_t a[4 + 1 + 48];
            size_t al = strlen(addr);
            /*
             * Qualcomm IMS DCM IDL specification (imsdatadaemon):
             * family_type 1 is CS_IPV4, 2 is CS_IPV6.
             * Sending 1 with IPv6 address causes "Family type mismatch"
             * and SIP 408 Request Timeout.
             */
            a[0] = (fam == 6) ? 2 : 1;
            a[1] = a[2] = a[3] = 0;
            a[4] = al;
            memcpy(a + 5, addr, al);
            sdu_tlv(&o, 0x11, a, 5 + al);
        }
        if (echo) sdu_tlv32(&o, 0x12, echo);
        if (sdu_send(fd, &from, &o)) printf("warning=dcm ind send: %s\n", strerror(errno));

        /* Also send QMI_IMS_DCM_ADDRESS_CHANGE_IND_V01 (0x24) as stock does */
        if (ok) {
            uint8_t a[4 + 1 + 48];
            size_t al = strlen(addr);
            a[0] = (fam == 6) ? 2 : 1;
            a[1] = a[2] = a[3] = 0;
            a[4] = al;
            memcpy(a + 5, addr, al);
            sdu_init(&o, 0x04, 0, 0x24);
            sdu_tlv(&o, 0x01, &pid, 1);
            sdu_tlv(&o, 0x10, a, 5 + al);
            sdu_send(fd, &from, &o);
        }
        fflush(stdout);
    }
    if (call_up) wds_stop(handle);
    pkt.cmd = QRTR_TYPE_DEL_SERVER;
    sendto(fd, &pkt, sizeof(pkt), 0, (void *)&ctrl, sizeof(ctrl));
    close(fd);
    return 0;
}

/* Open one QRTR client socket per service we use (WDS mandatory, WDA optional) */
static int qrtr_open_clients(void) {
    int lfd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (lfd < 0 || getsockname(lfd, (void *)&me, &sl) < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        if (lfd >= 0) close(lfd);
        return -1;
    }
    struct qsvc list[128];
    int n = 0;
    /* the modem publishes its services a few seconds after mission mode */
    for (int tries = 0; tries < 10 && !s_stop; tries++) {
        n = qrtr_lookup(lfd, me.sq_node, list, 128);
        int have = 0;
        for (int i = 0; i < n; i++)
            if (list[i].svc == SVC_WDS && list[i].node != me.sq_node) have = 1;
        if (have) break;
        sleep(1);
    }
    close(lfd);
    struct { uint8_t svc; struct qclient *c; } want[] = {{SVC_WDS, &s_qwds}, {SVC_WDA, &s_qwda}};
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
        for (int i = 0; i < n; i++) {
            if (list[i].svc != want[w].svc || list[i].node == me.sq_node) continue;
            int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0) break;
            want[w].c->fd = fd;
            want[w].c->node = list[i].node;
            want[w].c->port = list[i].port;
            printf("info=qrtr svc 0x%02x v%u at node %u port %u\n", list[i].svc, list[i].ver,
                   list[i].node, list[i].port);
            break;
        }
    }
    if (s_qwds.fd < 0) {
        printf("error=no WDS service on QRTR (is the modem in mission mode?)\n");
        return -1;
    }
    return 0;
}

/* kebab-dataconn --qrtr-list : print every service published on QRTR */
static int qrtr_list(int argc __attribute__((unused)), char **argv __attribute__((unused))) {
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me;
    socklen_t sl = sizeof(me);
    if (fd < 0 || getsockname(fd, (void *)&me, &sl) < 0) {
        printf("error=qrtr socket: %s\n", strerror(errno));
        if (fd >= 0) close(fd);
        return 1;
    }
    struct qsvc list[256];
    int n = qrtr_lookup(fd, me.sq_node, list, 256);
    for (int i = 0; i < n; i++)
        printf("svc 0x%03x v%u inst %u node %u port %u\n", list[i].svc, list[i].ver,
               list[i].inst, list[i].node, list[i].port);
    if (!n)
        printf("no services\n");
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--qrtr-list"))
        return qrtr_list(argc, argv);
    if (argc > 2 && !strcmp(argv[1], "--diag-msgs")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        return diag_msgs(argc, argv);
    }
    if (argc > 5 && (!strcmp(argv[1], "--qrtr-listen") || !strcmp(argv[1], "--qrtr-serve"))) {
        s_qrtr_ack = !strcmp(argv[1], "--qrtr-serve");
        setvbuf(stdout, NULL, _IOLBF, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        return qrtr_listen(argv);
    }
    if (argc > 3 && !strcmp(argv[1], "--diag-logs")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        return diag_logs(argv);
    }
    if (argc > 2 && !strcmp(argv[1], "--diag-nv")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return diag_nv(argc, argv);
    }
    if (argc > 3 && !strcmp(argv[1], "--qmi")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return qrtr_raw(argc, argv);
    }
    if (argc > 1 && !strcmp(argv[1], "--ims-dcm")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        signal(SIGTERM, on_signal);
        signal(SIGINT, on_signal);
        s_qrtr = 1;
        return ims_dcm(argc > 2 ? atoi(argv[2]) : 0);
    }
    if (argc > 1 && !strcmp(argv[1], "--qrtr")) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        int set_mode = (argc > 3 && !strcmp(argv[2], "--set-mode")) ? atoi(argv[3]) : -1;
        return qrtr_probe(set_mode);
    }
    if (argc > 1 && !strcmp(argv[1], "--probe")) {
        const char *dev = getenv("KEBAB_QMI_DEV");
        int set_mode = (argc > 3 && !strcmp(argv[2], "--set-mode")) ? atoi(argv[3]) : -1;
        setvbuf(stdout, NULL, _IOLBF, 0);
        s_fd = open(dev && *dev ? dev : QMI_DEV_DEFAULT, O_RDWR | O_CLOEXEC);
        if (s_fd < 0) {
            printf("error=open: %s\n", strerror(errno));
            return 1;
        }
        int rc = probe(set_mode);
        close(s_fd);
        return rc;
    }

    const char *apn = argc > 1 ? argv[1] : "";
    uint8_t fam = (argc > 2 && !strcmp(argv[2], "ipv6")) ? 6 : 4;
    const char *dev = getenv("KEBAB_QMI_DEV");
    char ifname[IFNAMSIZ];
    uint32_t handle = 0;
    int rc = 1, lost = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!dev || !*dev) dev = QMI_DEV_DEFAULT;

    /*
     * Opt-in QMAP probe: raw-IP mhi_hwip0 wedges after sustained traffic.
     * With KEBAB_QMAP=1 set WDA UL/DL aggregation to QMAP, bind this WDS
     * client to KEBAB_QMAP_MUX (default 1), and create the matching
     * rmnet_dataN link. The default raw-IP path is untouched.
     */
    {
        const char *q = getenv("KEBAB_QMAP");
        s_qmap = q && *q && strcmp(q, "0");
        if (s_qmap) {
            const char *qm = getenv("KEBAB_QMAP_MUX");
            if (qm && *qm) s_qmap_mux = (unsigned int)strtoul(qm, NULL, 0);
            if (!s_qmap_mux || s_qmap_mux > 255) s_qmap_mux = 1;
            if (qmap_prepare_link())
                return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (find_ifname(ifname, sizeof(ifname))) {
        printf("error=no mhi_hwip netdev (is the modem in mission mode?)\n");
        return 1;
    }
    const char *tr = getenv("KEBAB_QMI_TRANSPORT");
    s_qrtr = !(tr && !strcmp(tr, "qmux"));
    if (s_qrtr) {
        if (qrtr_open_clients()) goto out;
    } else {
        s_fd = open(dev, O_RDWR | O_CLOEXEC);
        if (s_fd < 0) {
            printf("error=open %s: %s\n", dev, strerror(errno));
            return 1;
        }
    }

    if (s_qrtr ? s_qwda.fd >= 0 : ctl_get_cid(SVC_WDA, &s_wda_cid) == 0) {
        if (wda_set_raw_ip(1) && wda_set_raw_ip(0))
            printf("warning=could not set raw-IP data format, continuing\n");
    } else {
        printf("warning=no WDA client, assuming modem default data format\n");
    }

    if (!s_qrtr && ctl_get_cid(SVC_WDS, &s_wds_cid)) {
        printf("error=allocate WDS client\n");
        goto out;
    }
    if (s_qrtr) {
        const char *ifs = getenv("KEBAB_EP_IFACE");
        wds_bind_data_port(ifs && *ifs ? strtoul(ifs, NULL, 0) : 4,
                           s_qmap ? s_qmap_mux : 0);
    }
    wds_set_family(fam);
    /* a cancelled or timed-out start may still come up: always tear down */
    if (wds_start(apn, fam, &handle)) goto stop;
    if (report_settings(ifname, fam)) goto stop;

    printf("ready\n");

    long next_check = now_ms() + 5000;
    while (!s_stop && !s_disconnected) {
        if (now_ms() >= next_check) {
            next_check = now_ms() + 5000;
            if (wds_call_lost(fam)) {
                /* still send a stop below: harmless if the modem already dropped it */
                lost = 1;
                printf("disconnected\n");
                fflush(stdout);
                break;
            }
        }
        struct resp r;
        uint8_t svc = SVC_WDS, cid, type;
        uint16_t msgid, txn;
        int ret = s_qrtr ? qrtr_read_msg(&s_qwds, &r, 1000, &msgid, &txn, &type)
                         : read_msg(&r, 1000, &svc, &cid, &msgid, &txn, &type);
        if (ret < 0) {
            printf("error=read: %s\n", strerror(errno));
            break;
        }
        if (ret > 0 && type == 4) handle_indication(svc, msgid, &r);
    }
    rc = (s_disconnected || lost) ? 1 : 0;

stop:
    if (!s_disconnected) wds_stop(handle);
    iface_clear_addrs(ifname);
    iface_setup(ifname, 0, 0);
out:
    if (s_qrtr) {
        /* closing a QRTR socket sends DEL_CLIENT: the modem drops its client state */
        if (s_qwds.fd >= 0) close(s_qwds.fd);
        if (s_qwda.fd >= 0) close(s_qwda.fd);
    } else {
        ctl_release_cid(SVC_WDS, s_wds_cid);
        ctl_release_cid(SVC_WDA, s_wda_cid);
        close(s_fd);
    }
    return rc;
}
