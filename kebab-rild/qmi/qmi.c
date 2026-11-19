/*
 * qmi.c — QMI-over-QRTR transport for kebab-rild.
 *
 * Framing: a bare QMI SDU (flags, transaction, message id, length, TLVs) in an
 * AF_QIPCRTR datagram addressed to the service's announced port. The QRTR
 * control socket is used for NEW_LOOKUP and stays subscribed, so modem
 * restarts are observed as DEL_SERVER/NEW_SERVER.
 */
#define LOG_TAG "kebab-rild"

#include "qmi.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/qrtr.h>
#include <log/log.h>

#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL 0
#endif

__attribute__((weak)) int qmi_test_transact(struct qmi_client *c, uint16_t msgid,
                                            const uint8_t *req, size_t reqlen,
                                            uint8_t *rsp, size_t rsp_cap,
                                            size_t *rsp_len)
{
    (void)c;
    (void)msgid;
    (void)req;
    (void)reqlen;
    (void)rsp;
    (void)rsp_cap;
    (void)rsp_len;
    return -ENOSYS;
}

__attribute__((weak)) bool qmi_test_is_up(struct qmi_client *c)
{
    (void)c;
    return false;
}

void qmi_test_deliver_indication(struct qmi_client *c, uint16_t msg,
                                 const uint8_t *tlvs, size_t len)
{
    if (c->ind_cb)
        c->ind_cb(c, msg, tlvs, len);
}

static void timespec_in(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

size_t qmi_put_tlv(uint8_t *buf, size_t off, uint8_t type,
                   const void *data, uint16_t len)
{
    buf[off++] = type;
    buf[off++] = (uint8_t)(len & 0xff);
    buf[off++] = (uint8_t)(len >> 8);
    if (len && data) {
        memcpy(buf + off, data, len);
        off += len;
    }
    return off;
}

size_t qmi_put_u8(uint8_t *buf, size_t off, uint8_t type, uint8_t v)
{
    return qmi_put_tlv(buf, off, type, &v, 1);
}

size_t qmi_put_u16(uint8_t *buf, size_t off, uint8_t type, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xff), (uint8_t)(v >> 8) };
    return qmi_put_tlv(buf, off, type, b, sizeof(b));
}

size_t qmi_put_u32(uint8_t *buf, size_t off, uint8_t type, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                     (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return qmi_put_tlv(buf, off, type, b, sizeof(b));
}

const uint8_t *qmi_find_tlv(const uint8_t *tlvs, size_t len, uint8_t type,
                            uint16_t *out_len)
{
    size_t off = 0;

    while (off + 3 <= len) {
        uint16_t l = (uint16_t)(tlvs[off + 1] | (tlvs[off + 2] << 8));

        if (off + 3 + l > len)
            break;
        if (tlvs[off] == type) {
            if (out_len)
                *out_len = l;
            return tlvs + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

static void client_set_up(struct qmi_client *c, uint32_t node, uint32_t port)
{
    bool changed;

    pthread_mutex_lock(&c->mu);
    changed = c->node != node || c->port != port;
    c->node = node;
    c->port = port;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    if (!changed)
        return;
    ALOGI("QMI svc 0x%02x %s (node %u port %u)", c->service,
          port ? "up" : "down", node, port);
    if (c->up_cb)
        c->up_cb(c, port != 0);
}

static void *client_thread(void *arg)
{
    struct qmi_client *c = arg;

    for (;;) {
        struct sockaddr_qrtr me, sq;
        socklen_t sl = sizeof(me);
        struct qrtr_ctrl_pkt pkt;
        int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);

        if (fd < 0 || getsockname(fd, (struct sockaddr *)&me, &sl) < 0) {
            ALOGE("QMI svc 0x%02x socket: %s", c->service, strerror(errno));
            if (fd >= 0)
                close(fd);
            sleep(5);
            continue;
        }
        c->fd = fd;
        client_set_up(c, 0, 0);

        memset(&pkt, 0, sizeof(pkt));
        pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
        pkt.server.service = c->service;
        memset(&sq, 0, sizeof(sq));
        sq.sq_family = AF_QIPCRTR;
        sq.sq_node = me.sq_node;
        sq.sq_port = QRTR_PORT_CTRL;
        if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq,
                   sizeof(sq)) < 0) {
            ALOGE("QMI svc 0x%02x lookup: %s", c->service, strerror(errno));
            close(fd);
            sleep(5);
            continue;
        }

        for (;;) {
            uint8_t buf[QMI_MAX_MSG];
            struct sockaddr_qrtr from;
            socklen_t fl = sizeof(from);
            struct pollfd pfd = { .fd = fd, .events = POLLIN };

            if (poll(&pfd, 1, 1000) <= 0)
                continue;
            ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fl);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                ALOGE("QMI svc 0x%02x recv: %s", c->service, strerror(errno));
                break;
            }
            if (from.sq_port == QRTR_PORT_CTRL) {
                struct qrtr_ctrl_pkt *cp = (struct qrtr_ctrl_pkt *)buf;

                if (n < (ssize_t)sizeof(*cp) ||
                    cp->server.service != c->service)
                    continue;
                if (cp->cmd == QRTR_TYPE_NEW_SERVER &&
                    cp->server.node != me.sq_node) {
                    client_set_up(c, cp->server.node, cp->server.port);
                } else if (cp->cmd == QRTR_TYPE_DEL_SERVER) {
                    /* Only the port we are actually using may take us down; a
                     * late DEL for the pre-restart instance must not knock out
                     * the freshly announced one. */
                    pthread_mutex_lock(&c->mu);
                    bool stale = c->port && cp->server.port != c->port;
                    pthread_mutex_unlock(&c->mu);

                    if (!stale)
                        client_set_up(c, cp->server.node, 0);
                }
                continue;
            }

            pthread_mutex_lock(&c->mu);
            if (n >= 7 && from.sq_node == c->node && from.sq_port == c->port &&
                (buf[0] & 0x02) &&
                (buf[1] | (buf[2] << 8)) == c->wait_txn &&
                (buf[3] | (buf[4] << 8)) == c->wait_msg) {
                size_t tl = (size_t)(buf[5] | (buf[6] << 8));

                if (tl > (size_t)n - 7)
                    tl = (size_t)n - 7;
                if (tl > sizeof(c->wait_rsp))
                    tl = sizeof(c->wait_rsp);
                memcpy(c->wait_rsp, buf + 7, tl);
                c->wait_len = tl;
                c->wait_done = true;
                pthread_cond_broadcast(&c->cv);
                pthread_mutex_unlock(&c->mu);
                continue;
            }
            pthread_mutex_unlock(&c->mu);

            if ((buf[0] & 0x04) && n >= 7 &&
                from.sq_node == c->node && from.sq_port == c->port) {
                size_t tl = (size_t)(buf[5] | (buf[6] << 8));
                uint16_t msg = buf[3] | (buf[4] << 8);

                if (tl > (size_t)n - 7)
                    tl = (size_t)n - 7;
                if (c->ind_cb)
                    c->ind_cb(c, msg, buf + 7, tl);
            }
        }

        client_set_up(c, 0, 0);
        close(fd);
        c->fd = -1;
        sleep(5);
    }
    return NULL;
}

int qmi_client_start(struct qmi_client *c, uint16_t service,
                     qmi_ind_cb ind_cb, qmi_up_cb up_cb)
{
    memset(c, 0, sizeof(*c));
    c->service = service;
    c->ind_cb = ind_cb;
    c->up_cb = up_cb;
    c->fd = -1;
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->req_mu, NULL);
    pthread_mutex_init(&c->snd_mu, NULL);
    pthread_cond_init(&c->cv, NULL);

    if (pthread_create(&c->thread, NULL, client_thread, c) != 0)
        return -EAGAIN;
    c->running = true;
    return 0;
}

int qmi_client_wait_up(struct qmi_client *c, int timeout_ms)
{
    struct timespec ts;
    int ret = 0;

    if (qmi_test_is_up(c))
        return 0;
    timespec_in(&ts, timeout_ms);
    pthread_mutex_lock(&c->mu);
    while (!c->port && ret == 0)
        ret = pthread_cond_timedwait(&c->cv, &c->mu, &ts);
    ret = c->port ? 0 : -ETIMEDOUT;
    pthread_mutex_unlock(&c->mu);
    return ret;
}

bool qmi_client_is_up(struct qmi_client *c)
{
    bool up;

    if (qmi_test_is_up(c))
        return true;
    pthread_mutex_lock(&c->mu);
    up = c->port != 0;
    pthread_mutex_unlock(&c->mu);
    return up;
}

int qmi_transact(struct qmi_client *c, uint16_t msgid,
                 const uint8_t *tlvs, size_t tlen,
                 uint8_t *rsp, size_t *rsp_len, int timeout_ms)
{
    struct sockaddr_qrtr sq;
    struct timespec ts;
    uint8_t buf[7 + 4096];
    uint16_t txn;
    int rc;

    if (tlen > 4096)
        return -E2BIG;

    {
        int trc = qmi_test_transact(c, msgid, tlvs, tlen, rsp,
                                    (rsp && rsp_len) ? *rsp_len : 0, rsp_len);

        if (trc != -ENOSYS)
            return trc;
    }

    pthread_mutex_lock(&c->req_mu);
    pthread_mutex_lock(&c->mu);
    if (!c->port) {
        pthread_mutex_unlock(&c->mu);
        pthread_mutex_unlock(&c->req_mu);
        return -ENOTCONN;
    }
    txn = ++c->txn;
    if (!txn)
        txn = ++c->txn;
    c->wait_txn = txn;
    c->wait_msg = msgid;
    c->wait_done = false;
    c->wait_len = 0;
    uint32_t node = c->node, port = c->port;
    int fd = c->fd;
    pthread_mutex_unlock(&c->mu);

    buf[0] = 0x00;
    buf[1] = (uint8_t)(txn & 0xff);
    buf[2] = (uint8_t)(txn >> 8);
    buf[3] = (uint8_t)(msgid & 0xff);
    buf[4] = (uint8_t)(msgid >> 8);
    buf[5] = (uint8_t)(tlen & 0xff);
    buf[6] = (uint8_t)(tlen >> 8);
    if (tlen && tlvs)
        memcpy(buf + 7, tlvs, tlen);

    memset(&sq, 0, sizeof(sq));
    sq.sq_family = AF_QIPCRTR;
    sq.sq_node = node;
    sq.sq_port = port;

    pthread_mutex_lock(&c->snd_mu);
    rc = sendto(fd, buf, 7 + tlen, 0, (struct sockaddr *)&sq, sizeof(sq));
    pthread_mutex_unlock(&c->snd_mu);
    if (rc < 0) {
        pthread_mutex_unlock(&c->req_mu);
        return -errno;
    }

    timespec_in(&ts, timeout_ms);
    pthread_mutex_lock(&c->mu);
    while (!c->wait_done) {
        rc = pthread_cond_timedwait(&c->cv, &c->mu, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    if (!c->wait_done) {
        pthread_mutex_unlock(&c->mu);
        pthread_mutex_unlock(&c->req_mu);
        return -ETIMEDOUT;
    }
    if (rsp && rsp_len) {
        size_t n = c->wait_len;

        if (n > *rsp_len)
            n = *rsp_len;
        memcpy(rsp, c->wait_rsp, n);
        *rsp_len = n;
    }
    size_t rlen = c->wait_len;
    uint8_t rbuf[QMI_MAX_MSG];

    memcpy(rbuf, c->wait_rsp, rlen);
    pthread_mutex_unlock(&c->mu);

    /* Result TLV 0x02: result u16 + error u16. Return the QMI error code. */
    uint16_t rl = 0;
    const uint8_t *res = qmi_find_tlv(rbuf, rlen, 0x02, &rl);

    rc = 0;
    if (res && rl >= 4) {
        uint16_t result = (uint16_t)(res[0] | (res[1] << 8));
        uint16_t error = (uint16_t)(res[2] | (res[3] << 8));

        if (result != 0)
            rc = error ? error : -EIO;
    }
    pthread_mutex_unlock(&c->req_mu);
    return rc;
}
