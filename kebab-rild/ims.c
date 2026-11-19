/*
 * ims.c — IMSA registration tracking (M6).
 *
 * Message ids verified on this modem (handover probes and stock analysis):
 *   0x0033 BIND                : TLV 0x10 u32 client id (0).
 *   0x0022 INDICATION_REGISTER : TLVs 0x10 and 0x11 = 1.
 *   0x0020 GET_REG_STATUS      : response TLV 0x12 u8: 0 none, 1 registering,
 *                                2 registered.
 *   0x0021 GET_SERVICE_STATUS  : response TLV 0x11 u8 (voice): 0 unavailable,
 *                                1 limited, 2 available.
 *   0x0023 / 0x0024            : registration / service status indications;
 *                                they only trigger a re-poll.
 */
#define LOG_TAG "kebab-rild"

#include "ims.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#define IMSA_BIND          0x0033
#define IMSA_REGISTER_IND  0x0022
#define IMSA_GET_REG       0x0020
#define IMSA_GET_SERVICES  0x0021

static volatile bool s_polling;
static pthread_mutex_t s_poll_mu = PTHREAD_MUTEX_INITIALIZER;

static void ims_notify_changed(int old_reg, int new_reg)
{
    if (old_reg != new_reg) {
        ALOGI("IMS: registration %d -> %d", old_reg, new_reg);
        kebab_unsol(RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED, NULL, 0);
    }
    state_update_readiness();
}

void ims_poll_once(void)
{
    uint8_t rsp[256];
    size_t rl;
    int rc, old, reg, srv = -1;
    uint16_t l = 0;
    const uint8_t *v;

    pthread_mutex_lock(&g_state.mu);
    old = g_state.ims_reg;
    pthread_mutex_unlock(&g_state.mu);

    rl = sizeof(rsp);
    rc = qmi_transact(&g_imsa, IMSA_GET_REG, NULL, 0, rsp, &rl, 5000);
    if (rc == 0) {
        v = qmi_find_tlv(rsp, rl, 0x12, &l);
        if (v && l >= 1) {
            reg = (v[0] == 2) ? 2 : (v[0] == 1 ? 1 : 0);
        } else {
            v = qmi_find_tlv(rsp, rl, 0x10, &l);
            reg = (v && l >= 1 && v[0]) ? 2 : 0;
        }

        rl = sizeof(rsp);
        rc = qmi_transact(&g_imsa, IMSA_GET_SERVICES, NULL, 0, rsp, &rl, 5000);
        v = rc == 0 ? qmi_find_tlv(rsp, rl, 0x11, &l) : NULL;
        if (v && l >= 1)
            srv = v[0];

        pthread_mutex_lock(&g_state.mu);
        if (!g_state.radio_on) {
            pthread_mutex_unlock(&g_state.mu);
            return;
        }
        g_state.ims_reg = reg;
        if (reg == 0)
            g_state.ims_voice_srv = 0;
        else if (srv >= 0)
            g_state.ims_voice_srv = srv;
        pthread_mutex_unlock(&g_state.mu);
        ims_notify_changed(old, reg);
    }
}

static void *ims_poll_thread(void *arg)
{
    const struct timespec tick = { .tv_sec = 10, .tv_nsec = 0 };

    (void)arg;
    while (qmi_client_is_up(&g_imsa)) {
        bool on;

        pthread_mutex_lock(&g_state.mu);
        on = g_state.radio_on;
        pthread_mutex_unlock(&g_state.mu);
        if (on)
            ims_poll_once();
        nanosleep(&tick, NULL);
    }
    pthread_mutex_lock(&s_poll_mu);
    s_polling = false;
    pthread_mutex_unlock(&s_poll_mu);
    return NULL;
}

static void ims_start_poll(void)
{
    pthread_t t;

    pthread_mutex_lock(&s_poll_mu);
    if (s_polling) {
        pthread_mutex_unlock(&s_poll_mu);
        return;
    }
    s_polling = true;
    pthread_mutex_unlock(&s_poll_mu);
    if (pthread_create(&t, NULL, ims_poll_thread, NULL) != 0) {
        pthread_mutex_lock(&s_poll_mu);
        s_polling = false;
        pthread_mutex_unlock(&s_poll_mu);
        ALOGE("IMS: cannot start poll thread");
        return;
    }
    pthread_detach(t);
}

static void *ims_bind_thread(void *arg)
{
    uint8_t req[16];
    size_t n = 0;

    (void)arg;
    if (qmi_client_wait_up(&g_imsa, 30000)) {
        ALOGE("IMS: service not up");
        return NULL;
    }
    for (int i = 0; i < 15 && qmi_client_is_up(&g_imsa); i++) {
        int rb, ri;

        n = qmi_put_u32(req, 0, 0x10, 0);
        rb = qmi_transact(&g_imsa, IMSA_BIND, req, n, NULL, NULL, 5000);
        n = qmi_put_u8(req, 0, 0x10, 1);
        n = qmi_put_u8(req, n, 0x11, 1);
        ri = qmi_transact(&g_imsa, IMSA_REGISTER_IND, req, n, NULL, NULL, 5000);
        if (rb == 0 && ri == 0)
            break;
        ALOGE("IMS: bind %d register-ind %d (attempt %d)", rb, ri, i + 1);
        sleep(1);
    }

    ALOGI("IMS: bound, polling registration");
    ims_start_poll();
    return NULL;
}

static void ims_ind_cb(struct qmi_client *c, uint16_t msg,
                       const uint8_t *tlvs, size_t len)
{
    (void)c;
    ALOGD("IMS: indication 0x%04x len %zu", msg, len);
    for (size_t off = 0; off + 3 <= len; ) {
        uint8_t type = tlvs[off];
        uint16_t tlen = (uint16_t)(tlvs[off + 1] | (tlvs[off + 2] << 8));
        off += 3;
        if (off + tlen > len) break;
        char hex[64] = "";
        for (int i = 0; i < tlen && i < 16; i++)
            sprintf(hex + 3 * i, "%02x ", tlvs[off + i]);
        ALOGI("IMS: ind 0x%04x TLV 0x%02x len %u: %s", msg, type, tlen, hex);
        off += tlen;
    }
    if (msg == 0x0023) { /* IMSA_REGISTRATION_STATUS_IND */
        uint16_t l = 0;
        /*
         * QMI IMSA 0x0023 (libqmi / modem spec):
         *   TLV 0x11 = IMS Registration Status (u32): 0 unreg, 1 registering, 2 registered, 3 limited.
         *   TLV 0x10 = IMS Registration Error Code (u16): 0 = NO_ERROR.
         *   TLV 0x12 = IMS Registration Error Message (string).
         *   TLV 0x13 = IMS Registration Technology (u32): 1 = WWAN.
         * DO NOT parse TLV 0x10 as registration status!
         */
        const uint8_t *v = qmi_find_tlv(tlvs, len, 0x11, &l);
        int reg = -1;

        if (v && l >= 4) {
            uint32_t s = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                         ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
            reg = (s == 2 || s == 3) ? 2 : (s == 1 ? 1 : 0);
        } else if (v && l >= 1) {
            reg = (v[0] == 2 || v[0] == 3) ? 2 : (v[0] == 1 ? 1 : 0);
        }
        if (reg >= 0) {
            int old;

            pthread_mutex_lock(&g_state.mu);
            if (!g_state.radio_on) {
                pthread_mutex_unlock(&g_state.mu);
                return;
            }
            old = g_state.ims_reg;
            g_state.ims_reg = reg;
            if (reg == 0)
                g_state.ims_voice_srv = 0;
            pthread_mutex_unlock(&g_state.mu);
            ims_notify_changed(old, reg);
        }
    } else if (msg == 0x0024) { /* IMSA_SERVICE_STATUS_IND */
        uint16_t l = 0;
        /*
         * QMI IMSA 0x0024:
         *   TLV 0x11 = Voice Service Status (u32): 0 unavailable, 1 limited, 2 available.
         *   TLV 0x16 = UE to TAS Service Status (u32): 2 = available.
         */
        const uint8_t *v = qmi_find_tlv(tlvs, len, 0x11, &l);
        if (!v)
            v = qmi_find_tlv(tlvs, len, 0x16, &l);

        int srv = -1;
        if (v && l >= 4) {
            uint32_t s = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                         ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
            srv = (s == 2) ? 2 : (s == 1 ? 1 : 0);
        } else if (v && l >= 1) {
            srv = (v[0] == 2) ? 2 : (v[0] == 1 ? 1 : 0);
        }
        if (srv >= 0) {
            /* Service availability is not registration: keep ims_reg fed only
             * by 0x0020/0x0023 (stock behaviour). */
            pthread_mutex_lock(&g_state.mu);
            if (!g_state.radio_on) {
                pthread_mutex_unlock(&g_state.mu);
                return;
            }
            g_state.ims_voice_srv = srv;
            pthread_mutex_unlock(&g_state.mu);
            state_update_readiness();
        }
    }
}

static void ims_up_cb(struct qmi_client *c, bool up)
{
    (void)c;
    if (up) {
        pthread_t t;

        if (pthread_create(&t, NULL, ims_bind_thread, NULL) == 0)
            pthread_detach(t);
    } else {
        int old;

        pthread_mutex_lock(&g_state.mu);
        old = g_state.ims_reg;
        g_state.ims_reg = 0;
        g_state.ims_voice_srv = 0;
        pthread_mutex_unlock(&g_state.mu);
        ims_notify_changed(old, 0);
    }
}

void ims_init(void)
{
    if (qmi_client_start(&g_imsa, QMI_SVC_IMSA, ims_ind_cb, ims_up_cb) != 0)
        ALOGE("IMS: client start failed");
}

bool ims_voip_registered(void)
{
    bool ok;

    pthread_mutex_lock(&g_state.mu);
    ok = (g_state.ims_reg == 2);
    pthread_mutex_unlock(&g_state.mu);
    return ok;
}

int ims_reg_status(void)
{
    int v;

    pthread_mutex_lock(&g_state.mu);
    v = g_state.ims_reg;
    pthread_mutex_unlock(&g_state.mu);
    return v;
}

void ims_force_unregistered(void)
{
    int old;

    pthread_mutex_lock(&g_state.mu);
    old = g_state.ims_reg;
    g_state.ims_reg = 0;
    g_state.ims_voice_srv = 0;
    pthread_mutex_unlock(&g_state.mu);
    ims_notify_changed(old, 0);
}
