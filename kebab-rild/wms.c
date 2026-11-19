/*
 * wms.c — SMS over QMI WMS.
 *
 * Proven wire semantics from the current reference-ril:
 *   0x20 RAW_SEND : TLV 0x01 {u8 format 6, u16 len, smsc+pdu}, TLV 0x13
 *                   sms_on_ims u8 = 1; response TLV 0x01 u16 message ref,
 *                   TLV 0x12 {u16 rp_cause, u8 tp_cause}.
 *   0x37 SEND_ACK : TLV 0x01 {u32 tid, u8 fmt 1, u8 success}, TLV 0x11
 *                   {u8 rp_cause, u8 tp_cause} on failure, TLV 0x12 ims.
 *   0x22 RAW_READ : TLV 0x01 {u8 storage, u32 index}, TLV 0x10 mode u8.
 *   0x24 DELETE   : same key; used only after the framework acks.
 *   0x48 transport: TLV 0x10 byte 0 == 1 means the IMS SMS transport is up.
 * MT arrives as indication 0x0001: TLV 0x11 transfer-route
 * {u8 ack_ind, u32 tid, u8 fmt, u16 len, pdu} with SMSC in 0x15 and
 * sms_on_ims in 0x16, or TLV 0x10 stored {u8 storage, u32 index}.
 */
#define LOG_TAG "kebab-rild"

#include "wms.h"
#include "ril_glue.h"
#include "state.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#define WMS_SET_EVENT_REPORT   0x0001
#define WMS_RAW_SEND           0x0020
#define WMS_RAW_READ           0x0022
#define WMS_DELETE             0x0024
#define WMS_SEND_ACK           0x0037
#define WMS_REGISTER_IND       0x0047
#define WMS_GET_TRANSPORT      0x0048
#define WMS_GET_SMSC           0x0034
#define WMS_SET_DOMAIN_PREF    0x0052

enum { ACK_NONE, ACK_TRANSFER, ACK_STORED };
struct wms_ack {
    uint8_t kind, ims, storage, mode, tag;
    uint32_t id;
};

#define ACKQ 8
static struct wms_ack s_ackq[ACKQ];
static int s_ackq_head, s_ackq_len;
static pthread_mutex_t s_ackq_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_transport_ims; /* 0 unknown/absent, 1 available */
static pthread_mutex_t s_smsc_mu = PTHREAD_MUTEX_INITIALIZER;
static char s_smsc[24];
static char s_smsc_type[4];

static int hex_to_bytes(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;
    unsigned v;

    while (hex && hex[0] && hex[1] && n < max) {
        if (sscanf(hex, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return (int)n;
}

static char *bytes_to_hex(const uint8_t *b, size_t len, const char *prefix)
{
    size_t pl = prefix ? strlen(prefix) : 0;
    char *s = malloc(pl + 2 * len + 1);
    size_t i;

    if (!s)
        return NULL;
    if (pl)
        memcpy(s, prefix, pl);
    for (i = 0; i < len; i++)
        snprintf(s + pl + 2 * i, 3, "%02x", b[i]);
    s[pl + 2 * len] = '\0';
    return s;
}

static void ack_push(uint8_t kind, uint32_t id, uint8_t ims, uint8_t storage,
                     uint8_t mode, uint8_t tag)
{
    pthread_mutex_lock(&s_ackq_mu);
    if (s_ackq_len < ACKQ) {
        struct wms_ack *a = &s_ackq[(s_ackq_head + s_ackq_len) % ACKQ];

        a->kind = kind;
        a->id = id;
        a->ims = ims;
        a->storage = storage;
        a->mode = mode;
        a->tag = tag;
        s_ackq_len++;
    }
    pthread_mutex_unlock(&s_ackq_mu);
}

static void deliver(const char *smsc_hex, const uint8_t *pdu, size_t len)
{
    /* Stored PDUs already carry the SMSC; transfer-route PDUs get it from
     * TLV 0x15. Stock sends the SMSC there as raw BCD bytes, other firmware
     * sends hex text; handle both (see handle_event_report). */
    char *hex = bytes_to_hex(pdu, len, smsc_hex && smsc_hex[0] ? smsc_hex : NULL);

    if (!hex)
        return;
    ALOGI("WMS: new SMS pdu len %zu", len);
    kebab_unsol(RIL_UNSOL_RESPONSE_NEW_SMS, hex, strlen(hex));
    free(hex);
}

/* Runs on a worker: RAW_READ must not transact from the client thread. */
static void *stored_read_thread(void *arg)
{
    struct wms_ack a = *(struct wms_ack *)arg;
    uint8_t req[16], rsp[1024];
    size_t n = 0, rl = sizeof(rsp);
    uint8_t key[5];
    int rc;

    free(arg);
    key[0] = a.storage;
    key[1] = (uint8_t)a.id;
    key[2] = (uint8_t)(a.id >> 8);
    key[3] = (uint8_t)(a.id >> 16);
    key[4] = (uint8_t)(a.id >> 24);
    n = qmi_put_tlv(req, n, 0x01, key, sizeof(key));
    n = qmi_put_u8(req, n, 0x10, a.mode);
    if (a.ims)
        n = qmi_put_u8(req, n, 0x11, 1); /* SMS on IMS */
    rc = qmi_transact(&g_wms, WMS_RAW_READ, req, n, rsp, &rl, 15000);
    if (rc) {
        ALOGE("WMS: raw read %u/%u failed %d", a.storage, a.id, rc);
        return NULL;
    }
    {
        uint16_t l = 0;
        const uint8_t *v = qmi_find_tlv(rsp, rl, 0x01, &l);

        if (v && l >= 4) {
            uint16_t dl = (uint16_t)(v[2] | (v[3] << 8));
            uint8_t tag = v[0];

            if (dl > l - 4)
                dl = (uint16_t)(l - 4);
            deliver(NULL, v + 4, dl);
            ack_push(ACK_STORED, a.id, a.ims, a.storage, a.mode, tag);
        } else {
            ALOGE("WMS: raw read %u/%u: no PDU", a.storage, a.id);
        }
    }
    return NULL;
}

static void handle_event_report(const uint8_t *tlvs, size_t len)
{
    uint16_t l = 0;
    const uint8_t *v;

    v = qmi_find_tlv(tlvs, len, 0x11, &l);
    if (v && l >= 8) {
        uint32_t tid = (uint32_t)v[1] | ((uint32_t)v[2] << 8) |
                       ((uint32_t)v[3] << 16) | ((uint32_t)v[4] << 24);
        uint16_t dl = (uint16_t)(v[6] | (v[7] << 8));
        uint16_t sl = 0, il = 0;
        const uint8_t *smsc = qmi_find_tlv(tlvs, len, 0x15, &sl);
        const uint8_t *ims = qmi_find_tlv(tlvs, len, 0x16, &il);
        char *smsc_hex = NULL;

        if (smsc && sl) {
            size_t hl = strnlen((const char *)smsc, sl);
            size_t k;
            int is_hex = hl > 0;

            for (k = 0; k < hl; k++)
                if (!isxdigit((unsigned char)smsc[k]))
                    is_hex = 0;
            if (is_hex)
                smsc_hex = strndup((const char *)smsc, hl);
            else
                smsc_hex = bytes_to_hex(smsc, sl, NULL);
        }

        if (dl > l - 8)
            dl = (uint16_t)(l - 8);
        deliver(smsc_hex, v + 8, dl);
        free(smsc_hex);
        ack_push(ACK_TRANSFER, tid, ims && il ? ims[0] : 0, 0, 0, 0);
        return;
    }

    v = qmi_find_tlv(tlvs, len, 0x10, &l);
    if (v && l >= 5) {
        uint16_t ml = 0, il = 0;
        const uint8_t *mode = qmi_find_tlv(tlvs, len, 0x12, &ml);
        const uint8_t *ims = qmi_find_tlv(tlvs, len, 0x16, &il);
        struct wms_ack a = {
            .kind = ACK_STORED,
            .storage = v[0],
            .id = (uint32_t)v[1] | ((uint32_t)v[2] << 8) |
                  ((uint32_t)v[3] << 16) | ((uint32_t)v[4] << 24),
            .mode = mode && ml ? mode[0] : 1,
            .ims = ims && il ? ims[0] : 0,
        };
        struct wms_ack *heap = malloc(sizeof(*heap));
        pthread_t t;

        if (!heap)
            return;
        *heap = a;
        if (pthread_create(&t, NULL, stored_read_thread, heap) != 0) {
            free(heap);
            return;
        }
        pthread_detach(t);
    }
}

static void wms_ind_cb(struct qmi_client *c, uint16_t msg,
                       const uint8_t *tlvs, size_t len)
{
    (void)c;
    if (msg == WMS_SET_EVENT_REPORT) {
        handle_event_report(tlvs, len);
    } else if (msg == 0x004B) { /* Transport Network Registration Status */
        uint16_t l = 0;
        const uint8_t *v = qmi_find_tlv(tlvs, len, 0x01, &l);

        if (v && l >= 1) {
            int up = v[0] != 0;

            if (up != (s_transport_ims == 1)) {
                s_transport_ims = up;
                ALOGI("WMS: IMS SMS transport %s", up ? "up" : "down");
                state_update_readiness();
            }
        }
    } else if (msg == 0x0046) { /* SMSC Address indication */
        uint16_t l = 0;
        const uint8_t *v = qmi_find_tlv(tlvs, len, 0x01, &l);

        if (v && l >= 4) {
            char type[4] = { (char)v[0], (char)v[1], (char)v[2], 0 };
            size_t dl = v[3];

            if (dl > (size_t)(l - 4))
                dl = (size_t)(l - 4);
            if (dl >= sizeof(s_smsc))
                dl = sizeof(s_smsc) - 1;
            pthread_mutex_lock(&s_smsc_mu);
            memcpy(s_smsc, v + 4, dl);
            s_smsc[dl] = '\0';
            memcpy(s_smsc_type, type, sizeof(s_smsc_type));
            pthread_mutex_unlock(&s_smsc_mu);
            ALOGI("WMS: SMSC %s type %s", s_smsc, s_smsc_type);
        }
    } else {
        ALOGD("WMS: indication 0x%04x", msg);
    }
}

static void wms_fetch_smsc(void)
{
    uint8_t rsp[128];
    size_t rl = sizeof(rsp), dl;
    uint16_t l = 0;
    const uint8_t *v, *d;

    if (qmi_transact(&g_wms, WMS_GET_SMSC, NULL, 0, rsp, &rl, 5000))
        return;
    v = qmi_find_tlv(rsp, rl, 0x01, &l);
    if (!v || l < 4)
        return;
    /* "145\r+917019075009": type, CR, then the number */
    for (dl = 0; dl < l && v[dl] != '\r'; dl++)
        ;
    d = v + dl + 1;
    dl = (size_t)(l - (dl + 1));
    if (dl && d[0] == '+') {
        d++;
        dl--;
    }
    if (!dl || dl >= sizeof(s_smsc))
        return;
    pthread_mutex_lock(&s_smsc_mu);
    memcpy(s_smsc, d, dl);
    s_smsc[dl] = '\0';
    memcpy(s_smsc_type, v, 3);
    s_smsc_type[3] = '\0';
    pthread_mutex_unlock(&s_smsc_mu);
    ALOGI("WMS: SMSC %s type %s (0x34)", s_smsc, s_smsc_type);
}

static volatile bool s_cfg_running;
static volatile bool s_reconfig;
static pthread_mutex_t s_cfg_mu = PTHREAD_MUTEX_INITIALIZER;

/* Transfer-route acks reference modem transaction ids that die with an SSR. */
static void wms_flush_transfer_acks(void)
{
    struct wms_ack kept[ACKQ];
    int kn = 0;

    pthread_mutex_lock(&s_ackq_mu);
    for (int i = 0; i < s_ackq_len; i++) {
        struct wms_ack *a = &s_ackq[(s_ackq_head + i) % ACKQ];

        if (a->kind != ACK_TRANSFER)
            kept[kn++] = *a;
    }
    for (int i = 0; i < kn; i++)
        s_ackq[i] = kept[i];
    s_ackq_head = 0;
    s_ackq_len = kn;
    pthread_mutex_unlock(&s_ackq_mu);
}

static void wms_do_config(void)
{
    uint8_t req[24], rsp[64];
    size_t rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;
    int rc;

    qmi_transact(&g_wms, WMS_SET_EVENT_REPORT,
                 req, qmi_put_u8(req, 0, 0x10, 1), NULL, NULL, 5000);
    {
        uint8_t ev[32];
        size_t evn = 0, indn = 0;
        uint8_t ind[40];

        /* Stock enables six event-report and eight indication flags. */
        for (uint8_t t = 0x10; t <= 0x15; t++)
            evn = qmi_put_u8(ev, evn, t, 1);
        qmi_transact(&g_wms, WMS_SET_EVENT_REPORT, ev, evn, NULL, NULL, 5000);
        for (uint8_t t = 0x10; t <= 0x17; t++)
            indn = qmi_put_u8(ind, indn, t, 1);
        qmi_transact(&g_wms, WMS_REGISTER_IND, ind, indn, NULL, NULL, 5000);
    }

    /* Client-scoped and lost on every modem restart. */
    qmi_transact(&g_wms, WMS_SET_DOMAIN_PREF,
                 req, qmi_put_u8(req, 0, 0x10, 1), NULL, NULL, 5000);

    rl = sizeof(rsp);
    rc = qmi_transact(&g_wms, WMS_GET_TRANSPORT, NULL, 0, rsp, &rl, 5000);
    v = rc == 0 ? qmi_find_tlv(rsp, rl, 0x10, &l) : NULL;
    s_transport_ims = (v && l >= 1 && v[0] == 1) ? 1 : 0;
    ALOGI("WMS: IMS SMS transport %s (err %d)", s_transport_ims ? "up" : "down",
          rc);
    wms_fetch_smsc();
    state_update_readiness();
}

static void *wms_config_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t rsp[64];
        size_t rl;
        uint16_t l = 0;
        const uint8_t *v;
        int rc, ticks = 0;

        if (qmi_client_wait_up(&g_wms, 20000)) {
            ALOGE("WMS: service not up");
            break;
        }
        wms_do_config();
        s_reconfig = false;

        while (qmi_client_is_up(&g_wms)) {
            const struct timespec tick = { .tv_sec = 1, .tv_nsec = 0 };

            nanosleep(&tick, NULL);
            if (!qmi_client_is_up(&g_wms))
                break;
            if (s_reconfig) {
                s_reconfig = false;
                wms_do_config();
                ticks = 0;
                continue;
            }
            {
                bool on;

                pthread_mutex_lock(&g_state.mu);
                on = g_state.radio_on;
                pthread_mutex_unlock(&g_state.mu);
                if (!on) {
                    ticks = 30; /* poll immediately once radio returns */
                    continue;
                }
            }
            if (++ticks < 30)
                continue;
            ticks = 0;
            {
                int old = s_transport_ims;

                rl = sizeof(rsp);
                rc = qmi_transact(&g_wms, WMS_GET_TRANSPORT, NULL, 0, rsp,
                                  &rl, 5000);
                v = rc == 0 ? qmi_find_tlv(rsp, rl, 0x10, &l) : NULL;
                s_transport_ims = (v && l >= 1 && v[0] == 1) ? 1 : 0;
                if (s_transport_ims != old) {
                    ALOGI("WMS: IMS SMS transport %s",
                          s_transport_ims ? "up" : "down");
                    if (s_transport_ims)
                        wms_fetch_smsc();
                    state_update_readiness();
                }
            }
        }

        /* Modem gone: transport flag, pending transfer acks and readiness
         * props are all invalid now. */
        s_transport_ims = 0;
        wms_flush_transfer_acks();
        state_update_readiness();

        pthread_mutex_lock(&s_cfg_mu);
        if (qmi_client_is_up(&g_wms) || s_reconfig) {
            s_reconfig = false;
            pthread_mutex_unlock(&s_cfg_mu);
            continue;
        }
        s_cfg_running = false;
        pthread_mutex_unlock(&s_cfg_mu);
        break;
    }
    return NULL;
}

static void wms_kick_config(void)
{
    bool spawn = false;

    pthread_mutex_lock(&s_cfg_mu);
    s_reconfig = true;
    if (!s_cfg_running) {
        s_cfg_running = true;
        spawn = true;
    }
    pthread_mutex_unlock(&s_cfg_mu);
    if (!spawn)
        return;
    {
        pthread_t t;

        if (pthread_create(&t, NULL, wms_config_thread, NULL) != 0) {
            pthread_mutex_lock(&s_cfg_mu);
            s_cfg_running = false;
            pthread_mutex_unlock(&s_cfg_mu);
            ALOGE("WMS: cannot start config thread");
            return;
        }
        pthread_detach(t);
    }
}

static void wms_up_cb(struct qmi_client *c, bool up)
{
    (void)c;
    if (up)
        wms_kick_config();
}

void wms_radio_changed(bool on)
{
    if (!on) {
        s_transport_ims = 0;
        wms_flush_transfer_acks();
        state_update_readiness();
        return;
    }
    wms_kick_config();
}

void wms_init(void)
{
    if (qmi_client_start(&g_wms, QMI_SVC_WMS, wms_ind_cb, wms_up_cb) != 0)
        ALOGE("WMS: client start failed");
}

bool wms_ims_transport(void)
{
    return s_transport_ims == 1;
}

static int smsc_to_bcd(uint8_t *dst, size_t max)
{
    char digits[24], type[4];
    uint8_t bcd[12];
    uint8_t toa;
    size_t dl, p = 0;

    pthread_mutex_lock(&s_smsc_mu);
    snprintf(digits, sizeof(digits), "%s", s_smsc);
    snprintf(type, sizeof(type), "%s", s_smsc_type);
    pthread_mutex_unlock(&s_smsc_mu);

    dl = strlen(digits);
    if (dl == 0 || dl > 20 || max < 2)
        return 0;
    for (size_t i = 0; i < dl; i++)
        if (!isdigit((unsigned char)digits[i]))
            return 0;
    for (size_t i = 0; i < dl; i += 2) {
        uint8_t lo = (uint8_t)(digits[i] - '0');
        uint8_t hi = (i + 1 < dl) ? (uint8_t)(digits[i + 1] - '0') : 0x0f;

        bcd[p++] = (uint8_t)((hi << 4) | lo);
    }
    if (p + 2 > max)
        return 0;
    toa = (strcmp(type, "145") == 0) ? 0x91 : 0x81;
    dst[0] = (uint8_t)(p + 1); /* TOA + BCD bytes follow the length */
    dst[1] = toa;
    memcpy(dst + 2, bcd, p);
    return (int)(p + 2);
}

bool wms_get_smsc(char *out, size_t max)
{
    char digits[24], type[4];

    if (!out || max < 2)
        return false;
    pthread_mutex_lock(&s_smsc_mu);
    snprintf(digits, sizeof(digits), "%s", s_smsc);
    snprintf(type, sizeof(type), "%s", s_smsc_type);
    pthread_mutex_unlock(&s_smsc_mu);
    if (!digits[0])
        return false;
    if (!strcmp(type, "145"))
        snprintf(out, max, "+%s", digits);
    else
        snprintf(out, max, "%s", digits);
    return true;
}

int wms_send_sms(const char *smsc_hex, const char *pdu_hex, bool on_ims,
                 int *msg_ref, int *rp_cause)
{
    uint8_t raw[400], req[512], rsp[256];
    size_t rl = sizeof(rsp);
    int sl, pl, rc;
    uint16_t l = 0;
    const uint8_t *v;

    if (smsc_hex && smsc_hex[0]) {
        sl = hex_to_bytes(smsc_hex, raw + 3, sizeof(raw) - 3);
    } else {
        /* Stock reads 0x34 immediately before every send; the SMSC can change
         * at runtime, so do not rely on the boot-time cache. */
        wms_fetch_smsc();
        sl = smsc_to_bcd(raw + 3, sizeof(raw) - 3);
        if (sl <= 0)
            sl = hex_to_bytes("00", raw + 3, sizeof(raw) - 3);
    }
    if (sl < 0)
        return -1;
    pl = hex_to_bytes(pdu_hex, raw + 3 + sl, sizeof(raw) - 3 - (size_t)sl);
    if (pl <= 0)
        return -1;
    raw[0] = 0x06;
    raw[1] = (uint8_t)((sl + pl) & 0xff);
    raw[2] = (uint8_t)((sl + pl) >> 8);

    size_t n = qmi_put_tlv(req, 0, 0x01, raw, (uint16_t)(3 + sl + pl));

    n = qmi_put_u8(req, n, 0x13, on_ims ? 1 : 0);
    rc = qmi_transact(&g_wms, WMS_RAW_SEND, req, n, rsp, &rl, 30000);
    if (rc == 0) {
        v = qmi_find_tlv(rsp, rl, 0x01, &l);
        *msg_ref = (v && l >= 2) ? (v[0] | (v[1] << 8)) : 0;
        ALOGI("WMS: sent, ref %d", *msg_ref);
        return 0;
    }
    v = qmi_find_tlv(rsp, rl, 0x12, &l);
    if (rc > 0 && v && l >= 3) {
        *rp_cause = v[0] | (v[1] << 8);
        ALOGE("WMS: rejected qmi %d rp %d tp 0x%02x", rc, *rp_cause, v[2]);
        return 1;
    }
    if (rc == 70 || rc == -ETIMEDOUT) {
        ALOGE("WMS: retryable (error %d)", rc);
        return 2;
    }
    ALOGE("WMS: send failed %d", rc);
    return -1;
}

bool wms_ack_sms(int success, int fail_cause)
{
    struct wms_ack a;
    uint8_t req[32];
    size_t n = 0;

    pthread_mutex_lock(&s_ackq_mu);
    if (!s_ackq_len) {
        pthread_mutex_unlock(&s_ackq_mu);
        return false;
    }
    a = s_ackq[s_ackq_head];
    s_ackq_head = (s_ackq_head + 1) % ACKQ;
    s_ackq_len--;
    pthread_mutex_unlock(&s_ackq_mu);

    if (a.kind == ACK_STORED) {
        if (success) {
            /* Delete 0x24: separate storage/index/tag/mode TLVs. */
            n = qmi_put_u8(req, n, 0x01, a.storage);
            n = qmi_put_u32(req, n, 0x10, a.id);
            n = qmi_put_u8(req, n, 0x11, a.tag);
            n = qmi_put_u8(req, n, 0x12, a.mode);
            qmi_transact(&g_wms, WMS_DELETE, req, n, NULL, NULL, 5000);
        } else {
            ALOGE("WMS: kept stored %u/%u after reject", a.storage, a.id);
        }
        return true;
    }
    if (a.kind == ACK_TRANSFER) {
        uint8_t ack[6] = { (uint8_t)a.id, (uint8_t)(a.id >> 8),
                           (uint8_t)(a.id >> 16), (uint8_t)(a.id >> 24),
                           0x01, (uint8_t)(success ? 1 : 0) };
        uint8_t fail[2] = { (uint8_t)(fail_cause ? fail_cause : 41), 0xff };

        n = qmi_put_tlv(req, n, 0x01, ack, sizeof(ack));
        if (!success)
            n = qmi_put_tlv(req, n, 0x10, fail, sizeof(fail)); /* TLV 0x10 = 3GPP failure cause */
        n = qmi_put_u8(req, n, 0x12, a.ims);
        qmi_transact(&g_wms, WMS_SEND_ACK, req, n, NULL, NULL, 5000);
        return true;
    }
    return true;
}
