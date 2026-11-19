/*
 * dcm.c — QMI IMS DCM server (service 0x302), optional replacement for
 * kebab-dataconn --ims-dcm.
 *
 * Published like any QRTR server: bind an AF_QIPCRTR socket, announce with
 * QRTR_TYPE_NEW_SERVER (service 0x302, instance 1). Handled messages:
 *   PDP activate  (0x20): start an IMS PDN over WDS, answer with pdp id/cookie/
 *                         echo, then send the 0x20 indication with the address
 *                         (and the 0x24 address-change indication, as stock).
 *   PDP deactivate(0x21): stop the PDN, answer success.
 * Everything else is acknowledged with success, matching imsdatadaemon.
 *
 * Enabled only with persist.vendor.kebab.dcm_in_rild=1; otherwise
 * kebab-dataconn keeps owning 0x302.
 */
#define LOG_TAG "kebab-rild"

#include "dcm.h"
#include "state.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <linux/qrtr.h>
#include <log/log.h>

#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL 0
#endif

#define DCM_SVC            0x302
#define DCM_PDP_ID         1
#define WDS_START_NETWORK  0x0020
#define WDS_STOP_NETWORK   0x0021
#define WDS_GET_SETTINGS   0x002D

static int s_ims_handle;
static int s_ims_up;

static size_t sdu_begin(uint8_t *b, uint8_t flags, uint16_t txn, uint16_t msgid)
{
    b[0] = flags;
    b[1] = (uint8_t)(txn & 0xff);
    b[2] = (uint8_t)(txn >> 8);
    b[3] = (uint8_t)(msgid & 0xff);
    b[4] = (uint8_t)(msgid >> 8);
    b[5] = 0;
    b[6] = 0;
    return 7;
}

static int sdu_send(int fd, const struct sockaddr_qrtr *to, uint8_t *b, size_t len)
{
    uint16_t tl = (uint16_t)(len - 7);

    b[5] = (uint8_t)(tl & 0xff);
    b[6] = (uint8_t)(tl >> 8);
    return sendto(fd, b, len, 0, (const void *)to, sizeof(*to)) < 0 ? -1 : 0;
}

static size_t sdu_result(uint8_t *b, size_t len, uint16_t result, uint16_t error)
{
    uint8_t r[4] = { (uint8_t)result, (uint8_t)(result >> 8),
                     (uint8_t)error, (uint8_t)(error >> 8) };

    return qmi_put_tlv(b, len, 0x02, r, sizeof(r));
}

static int dcm_get_addr(uint8_t fam, char *out, size_t outlen)
{
    uint8_t req[8], rsp[1024];
    size_t n, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;

    n = qmi_put_u32(req, 0, 0x10, 0x000fffffu);
    if (qmi_transact(&g_wds, WDS_GET_SETTINGS, req, n, rsp, &rl, 10000))
        return -1;
    if (fam == 6 && (v = qmi_find_tlv(rsp, rl, 0x25, &l)) && l >= 17)
        return inet_ntop(AF_INET6, v, out, outlen) ? 0 : -1;
    if (fam == 4 && (v = qmi_find_tlv(rsp, rl, 0x1E, &l)) && l >= 4) {
        struct in_addr a = { .s_addr = htonl((uint32_t)v[0] |
                                             ((uint32_t)v[1] << 8) |
                                             ((uint32_t)v[2] << 16) |
                                             ((uint32_t)v[3] << 24)) };

        return inet_ntop(AF_INET, &a, out, outlen) ? 0 : -1;
    }
    return -1;
}

static int dcm_start_pdn(uint8_t fam)
{
    uint8_t req[24], rsp[256];
    size_t n = 0, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;

    n = qmi_put_tlv(req, n, 0x14, "ims", 3);
    n = qmi_put_u8(req, n, 0x19, fam);
    if (qmi_transact(&g_wds, WDS_START_NETWORK, req, n, rsp, &rl, 60000) != 0)
        return -1;
    v = qmi_find_tlv(rsp, rl, 0x01, &l);
    if (!v || l < 4)
        return -1;
    s_ims_handle = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    s_ims_up = s_ims_handle != 0;
    return s_ims_up ? 0 : -1;
}

static void dcm_stop_pdn(void)
{
    uint8_t req[8];

    if (!s_ims_up)
        return;
    qmi_transact(&g_wds, WDS_STOP_NETWORK,
                 req, qmi_put_u32(req, 0, 0x01, (uint32_t)s_ims_handle),
                 NULL, NULL, 10000);
    s_ims_up = 0;
    s_ims_handle = 0;
}

static void dcm_activate(int fd, const struct sockaddr_qrtr *from,
                         uint16_t txn, const uint8_t *b, size_t len)
{
    uint8_t out[256];
    char apn[101] = "";
    uint32_t cookie = 0, echo = 0, family = 1;
    uint8_t fam, pid = DCM_PDP_ID;
    char addr[64] = "";
    size_t end = 7 + (size_t)(b[5] | (b[6] << 8));
    size_t off;
    const uint8_t *v;
    int ok;

    if (end > len)
        end = len;
    for (off = 7; off + 3 <= end;) {
        uint16_t tl = (uint16_t)(b[off + 1] | (b[off + 2] << 8));

        if (off + 3 + tl > end)
            break;
        v = b + off + 3;
        if (b[off] == 0x01 && tl >= 1 && v[0] <= 100 && tl >= 1u + v[0] + 12) {
            memcpy(apn, v + 1, v[0]);
            apn[v[0]] = 0;
            family = v[1 + v[0] + 8] | (v[1 + v[0] + 9] << 8) |
                      (v[1 + v[0] + 10] << 16) | (v[1 + v[0] + 11] << 24);
        } else if (b[off] == 0x10 && tl >= 4) {
            cookie = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
        } else if (b[off] == 0x13 && tl >= 4) {
            echo = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
        }
        off += 3 + tl;
    }
    fam = family == 1 ? 6 : 4;
    ALOGI("DCM: ims activate apn '%s' ipv%u", apn, fam);

    /* Response first (pdp id, cookie, echo), then the address indication. */
    {
        size_t n = sdu_begin(out, 0x02, txn, 0x20);

        n = sdu_result(out, n, 0, 0);
        n = qmi_put_u8(out, n, 0x10, pid);
        n = qmi_put_u32(out, n, 0x11, cookie);
        n = qmi_put_u32(out, n, 0x12, echo);
        sdu_send(fd, from, out, n);
    }

    ok = s_ims_up;
    if (!ok)
        ok = dcm_start_pdn(fam) == 0;
    if (ok && dcm_get_addr(fam, addr, sizeof(addr)) != 0)
        ok = 0;
    ALOGI("DCM: ims pdp %s %s", ok ? "up" : "failed", addr);

    {
        size_t n = sdu_begin(out, 0x04, 0, 0x20);
        uint8_t a[4 + 1 + 48];
        size_t al = strlen(addr);

        n = sdu_result(out, n, ok ? 0 : 1, ok ? 0 : 14 /* CALL_FAILED */);
        n = qmi_put_u8(out, n, 0x01, pid);
        n = qmi_put_u32(out, n, 0x10, cookie);
        if (ok) {
            a[0] = (fam == 6) ? 2 : 1; /* family type: 1 v4, 2 v6 */
            a[1] = a[2] = a[3] = 0;
            a[4] = (uint8_t)al;
            memcpy(a + 5, addr, al);
            n = qmi_put_tlv(out, n, 0x11, a, (uint16_t)(5 + al));
        }
        if (echo)
            n = qmi_put_u32(out, n, 0x12, echo);
        sdu_send(fd, from, out, n);

        if (ok) {
            n = sdu_begin(out, 0x04, 0, 0x24); /* address change */
            n = qmi_put_u8(out, n, 0x01, pid);
            n = qmi_put_tlv(out, n, 0x10, a, (uint16_t)(5 + al));
            sdu_send(fd, from, out, n);
        }
    }
}

static void *dcm_thread(void *arg)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_qrtr me, ctrl;
    socklen_t sl = sizeof(me);
    struct qrtr_ctrl_pkt pkt;

    (void)arg;
    /* Publication does not need WDS, but activations do; give the data module
     * time to bring the service up before the modem starts asking. */
    qmi_client_wait_up(&g_wds, 30000);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&me, &sl) < 0) {
        ALOGE("DCM: socket: %s", strerror(errno));
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_SERVER;
    pkt.server.service = DCM_SVC;
    pkt.server.instance = 1;
    pkt.server.node = me.sq_node;
    pkt.server.port = me.sq_port;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node = me.sq_node;
    ctrl.sq_port = QRTR_PORT_CTRL;
    if (sendto(fd, &pkt, sizeof(pkt), 0, (void *)&ctrl, sizeof(ctrl)) < 0) {
        ALOGE("DCM: publish: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    ALOGI("DCM: published service 0x302 at node %u port %u", me.sq_node,
          me.sq_port);

    for (;;) {
        uint8_t b[2048], out[128];
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        ssize_t len;
        uint16_t txn, msgid;

        if (poll(&pfd, 1, 1000) <= 0)
            continue;
        len = recvfrom(fd, b, sizeof(b), MSG_DONTWAIT, (void *)&from, &fl);
        if (len < 7 || from.sq_port == QRTR_PORT_CTRL || b[0] != 0x00)
            continue;
        txn = (uint16_t)(b[1] | (b[2] << 8));
        msgid = (uint16_t)(b[3] | (b[4] << 8));
        if (msgid == 0x20) {
            dcm_activate(fd, &from, txn, b, (size_t)len);
        } else {
            size_t n = sdu_begin(out, 0x02, txn, msgid);

            n = sdu_result(out, n, 0, 0);
            if (msgid == 0x21) {
                dcm_stop_pdn();
                ALOGI("DCM: ims pdp deactivate");
            }
            sdu_send(fd, &from, out, n);
        }
    }
    return NULL;
}

int dcm_init(void)
{
    char prop[PROPERTY_VALUE_MAX];
    pthread_t t;

    if (property_get("persist.vendor.kebab.dcm_in_rild", prop, "0") <= 0 ||
        strcmp(prop, "1") != 0) {
        ALOGI("DCM: in-daemon server disabled (kebab-dataconn owns 0x302)");
        return 0;
    }
    /* The WDS client is owned by data.c and already running. */
    if (pthread_create(&t, NULL, dcm_thread, NULL) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}
