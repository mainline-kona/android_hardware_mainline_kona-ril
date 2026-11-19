/*
 * nas.c — NAS serving system, signal and system selection.
 *
 * Wire layouts verified against the current reference-ril on this modem:
 *   0x24 Get Serving System (no input)
 *        TLV 0x01: u8 reg_state, u8 cs, u8 ps, u8 n_radio, u8 radio_if[]
 *        TLV 0x12: u16 mcc, u16 mnc, string
 *   0x4F Get Signal Info (no input)
 *        TLV 0x14 LTE: s8 rssi, s8 rsrq, s16 rsrp(0.1 dBm), s16 snr(0.1 dB)
 *        TLV 0x17 NR : s16 rsrp(dBm), s16 snr(0.1 dB)
 *        TLV 0x18 NR : s16 rsrq(dB)
 *   0x33 Set System Selection Preference
 *        TLV 0x11 mode preference u16 0x0050 (LTE|NR5G)
 *        TLV 0x10 network selection u8 0 (automatic)
 *        TLV 0x12 service domain u32 2 (PS only)
 *        TLV 0x17 change duration u8 1 (immediate)
 */
#define LOG_TAG "kebab-rild"

#include "nas.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#define NAS_GET_SERVING_SYSTEM 0x0024
#define NAS_GET_SIGNAL_INFO    0x004F
#define NAS_SET_SYS_SEL_PREF   0x0033
#define NAS_SET_EVENT_REPORT   0x0002
#define NAS_INDICATION_REGISTER 0x0003
#define NAS_GET_PLMN_NAME      0x0044

static volatile bool s_refreshing;

static void nas_parse_serving(const uint8_t *tlvs, size_t len)
{
    uint16_t l = 0;
    const uint8_t *v;
    int reg = 0, ps = 0, rif = 0;

    v = qmi_find_tlv(tlvs, len, 0x01, &l);
    if (v && l >= 5) {
        reg = v[0];
        ps = v[2];
        if (l >= 6 && v[4] >= 1)
            rif = v[5];
        else
            rif = v[4];
    }
    pthread_mutex_lock(&g_state.mu);
    g_state.reg_state = reg;
    g_state.ps_state = ps;
    g_state.radio_if = rif;
    g_state.serving_ok = true;
    pthread_mutex_unlock(&g_state.mu);

    v = qmi_find_tlv(tlvs, len, 0x10, &l); /* roaming indicator */
    if (v && l >= 1) {
        pthread_mutex_lock(&g_state.mu);
        g_state.roaming = v[0] != 0;
        pthread_mutex_unlock(&g_state.mu);
    }

    v = qmi_find_tlv(tlvs, len, 0x12, &l);
    if (v && l >= 4) {
        int mcc = v[0] | (v[1] << 8);
        int mnc = v[2] | (v[3] << 8);

        if (mcc > 0 && mcc < 1000 && mnc < 1000) {
            pthread_mutex_lock(&g_state.mu);
            g_state.mcc = mcc;
            g_state.mnc = mnc;
            g_state.mnc_len = mnc > 99 ? 3 : 2;
            g_state.op_name[0] = '\0';
            if (l > 5) {
                size_t dl = v[4];

                if (dl > l - 5)
                    dl = l - 5;
                if (dl >= sizeof(g_state.op_name))
                    dl = sizeof(g_state.op_name) - 1;
                memcpy(g_state.op_name, v + 5, dl);
                g_state.op_name[dl] = '\0';
            }
            pthread_mutex_unlock(&g_state.mu);
        }
    }
}

/* The modem fills TLVs for every RAT it knows and uses 0x8000 for the ones
 * without a serving cell. Values are dBm for rsrp (LTE may also be 0.1 dBm). */
static bool signal_rsrp_valid(int v)
{
    int a = v < 0 ? -v : v;

    return (a >= 44 && a <= 140) || (a >= 440 && a <= 1400);
}

static void nas_parse_signal(const uint8_t *tlvs, size_t len)
{
    uint16_t l = 0;
    const uint8_t *v;
    int rssi = 0, rsrp = 0, rsrq = 0, snr = 0;
    bool have = false;

    int rat = 0;

    v = qmi_find_tlv(tlvs, len, 0x14, &l);
    if (v && l >= 6) {
        int r = (int16_t)(v[2] | (v[3] << 8));

        if (signal_rsrp_valid(r)) {
            rssi = (int8_t)v[0];
            rsrq = (int8_t)v[1];
            rsrp = r;
            snr = (int16_t)(v[4] | (v[5] << 8));
            rat = 1; /* LTE, rsrp in 0.1 dBm */
            have = true;
        }
    }
    v = qmi_find_tlv(tlvs, len, 0x17, &l);
    if (v && l >= 4) {
        int r = (int16_t)(v[0] | (v[1] << 8));

        /* On an LTE cell 0x17/0x18 are present with the 0x8000 sentinel; keep
         * the LTE values instead of letting the empty NR TLV clobber them. */
        if (signal_rsrp_valid(r)) {
            /* NR 5G: rsrp is dBm, snr is 0.1 dB; rsrq in TLV 0x18. */
            rsrp = r;
            snr = (int16_t)(v[2] | (v[3] << 8));
            v = qmi_find_tlv(tlvs, len, 0x18, &l);
            rsrq = (v && l >= 2) ? (int16_t)(v[0] | (v[1] << 8)) : 0;
            rat = 2;
            have = true;
        }
    }
    if (!have)
        return;

    pthread_mutex_lock(&g_state.mu);
    g_state.rssi = rssi;
    g_state.rsrp = rsrp;
    g_state.rsrq = rsrq;
    g_state.snr = snr;
    g_state.sig_rat = rat;
    g_state.signal_ok = true;
    pthread_mutex_unlock(&g_state.mu);
}

static void eons_copy(char *dst, size_t max, const uint8_t *src, size_t len)
{
    size_t n = strnlen((const char *)src, len);

    if (n >= max)
        n = max - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* QMI NAS EONS PLMN name (TLV 0x10):
 *   {u8 spn_enc, u8 spn_len, spn[],
 *    u8 short_enc, u8 country, u8 spare, u8 short_len, short[],
 *    u8 long_enc,  u8 country, u8 spare, u8 long_len,  long[]} */
static void nas_parse_eons(const uint8_t *v, uint16_t len, char *spn,
                           size_t spnm, char *sh, size_t shm, char *ln,
                           size_t lnm)
{
    size_t off = 0;
    uint8_t enc, n;

    spn[0] = sh[0] = ln[0] = '\0';
    if (off + 2 > len)
        return;
    enc = v[off++];
    n = v[off++];
    if (off + n > len)
        n = (uint8_t)(len - off);
    if (enc == 0 && n)
        eons_copy(spn, spnm, v + off, n);
    off += n;
    if (off + 4 <= len) {
        enc = v[off];
        n = v[off + 3];
        off += 4;
        if (off + n > len)
            n = (uint8_t)(len - off);
        if (enc == 0 && n)
            eons_copy(sh, shm, v + off, n);
        off += n;
    }
    if (off + 4 <= len) {
        enc = v[off];
        n = v[off + 3];
        off += 4;
        if (off + n > len)
            n = (uint8_t)(len - off);
        if (enc == 0 && n)
            eons_copy(ln, lnm, v + off, n);
    }
}

static void nas_fetch_plmn_name_sync(void)
{
    uint8_t req[32], rsp[256];
    uint8_t plmn[4];
    size_t n = 0, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;
    int mcc, mnc, rif;
    char spn[32], sh[40], ln[40];

    pthread_mutex_lock(&g_state.mu);
    mcc = g_state.mcc;
    mnc = g_state.mnc;
    rif = g_state.radio_if;
    pthread_mutex_unlock(&g_state.mu);
    if (mcc <= 0)
        return;
    plmn[0] = (uint8_t)(mcc & 0xff);
    plmn[1] = (uint8_t)(mcc >> 8);
    plmn[2] = (uint8_t)(mnc & 0xff);
    plmn[3] = (uint8_t)(mnc >> 8);
    n = qmi_put_tlv(req, n, 0x01, plmn, sizeof(plmn));
    n = qmi_put_u8(req, n, 0x10, 1);              /* suppress SIM error */
    n = qmi_put_u8(req, n, 0x15, (uint8_t)rif);   /* current RAT */
    if (qmi_transact(&g_nas, NAS_GET_PLMN_NAME, req, n, rsp, &rl, 5000))
        return;
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (!v)
        return;
    nas_parse_eons(v, l, spn, sizeof(spn), sh, sizeof(sh), ln, sizeof(ln));
    pthread_mutex_lock(&g_state.mu);
    g_state.op_name[0] = '\0';
    g_state.op_short[0] = '\0';
    g_state.eons_ok = false;
    if (ln[0] || sh[0]) {
        snprintf(g_state.op_name, sizeof(g_state.op_name), "%s",
                 ln[0] ? ln : sh);
        snprintf(g_state.op_short, sizeof(g_state.op_short), "%s",
                 sh[0] ? sh : ln);
        g_state.eons_ok = true;
    }
    pthread_mutex_unlock(&g_state.mu);
    if (ln[0] || sh[0] || spn[0])
        ALOGI("NAS: EONS spn '%s' short '%s' long '%s'", spn, sh, ln);
}

static void nas_refresh_sync(void)
{
    uint8_t rsp[1024];
    size_t rl;
    int rc;

    rl = sizeof(rsp);
    rc = qmi_transact(&g_nas, NAS_GET_SERVING_SYSTEM, NULL, 0, rsp, &rl, 5000);
    if (rc == 0) {
        nas_parse_serving(rsp, rl);
        nas_fetch_plmn_name_sync();
    } else {
        ALOGE("NAS: get serving system error %d", rc);
    }

    rl = sizeof(rsp);
    rc = qmi_transact(&g_nas, NAS_GET_SIGNAL_INFO, NULL, 0, rsp, &rl, 5000);
    if (rc == 0)
        nas_parse_signal(rsp, rl);
    else
        ALOGE("NAS: get signal info error %d", rc);
}

static void *nas_refresh_thread(void *arg)
{
    (void)arg;
    if (qmi_client_wait_up(&g_nas, 20000)) {
        ALOGE("NAS: service not up");
        goto out;
    }
    nas_refresh_sync();
out:
    s_refreshing = false;
    return NULL;
}

void nas_build_signal_strength(RIL_SignalStrength_v12 *ss)
{
    int rssi = 0, rsrp = 0, rsrq = 0, snr = 0, rat;

    /* INT_MAX is the RIL sentinel for invalid; not 0xff. */
    for (size_t i = 0; i < sizeof(*ss) / sizeof(int32_t); i++)
        ((int32_t *)ss)[i] = INT_MAX;
    ss->GW_SignalStrength.signalStrength = 99;
    ss->GW_SignalStrength.bitErrorRate = 99;
    ss->WCDMA_SignalStrength.signalStrength = 99;
    ss->WCDMA_SignalStrength.bitErrorRate = 99;

    if (!nas_get_signal(&rssi, &rsrp, &rsrq, &snr) || !rsrp)
        return;
    pthread_mutex_lock(&g_state.mu);
    rat = g_state.sig_rat;
    pthread_mutex_unlock(&g_state.mu);

    if (rat == 2) {
        /* NR: rsrp already dBm (negative); RIL wants the magnitude. */
        int m = rsrp < 0 ? -rsrp : rsrp;
        int q = rsrq < 0 ? -rsrq : rsrq;

        if (m >= 44 && m <= 140)
            ss->NR_SignalStrength.ssRsrp = m;
        if (q >= 3 && q <= 20)
            ss->NR_SignalStrength.ssRsrq = q;
        if (snr >= -230 && snr <= 400)
            ss->NR_SignalStrength.ssSinr = snr / 10;
        else if (snr >= -23 && snr <= 40)
            ss->NR_SignalStrength.ssSinr = snr;
    } else {
        /* LTE: TLV 0x14 rsrp is 0.1 dBm; tolerate plain dBm too. */
        int dbm = (rsrp < -200 || rsrp > 200) ? rsrp / 10 : rsrp;
        int asu = (rssi + 113) / 2;
        int q = rsrq < 0 ? -rsrq : rsrq;

        ss->LTE_SignalStrength.signalStrength =
            asu < 0 ? 0 : asu > 31 ? 31 : asu;
        if (dbm <= -44 && dbm >= -140)
            ss->LTE_SignalStrength.rsrp = -dbm;
        if (q >= 3 && q <= 20)
            ss->LTE_SignalStrength.rsrq = q;
        if (snr >= -200 && snr <= 300)
            ss->LTE_SignalStrength.rssnr = snr;
    }
}

static void nas_emit_signal(void)
{
    RIL_SignalStrength_v12 ss;

    nas_build_signal_strength(&ss);
    kebab_unsol(RIL_UNSOL_SIGNAL_STRENGTH, &ss, sizeof(ss));
}

static volatile bool s_poll_running;
static pthread_mutex_t s_poll_mu = PTHREAD_MUTEX_INITIALIZER;

/* Periodic refresh: without it registration/signal would only change when the
 * framework happens to ask, and the modem is not asked to emit NAS URCs. */
static void *nas_poll_thread(void *arg)
{
    const struct timespec tick = { .tv_sec = 10, .tv_nsec = 0 };
    int last_reg = -1, last_rif = -1, last_mcc = -1, last_mnc = -1;

    (void)arg;
    if (qmi_client_wait_up(&g_nas, 30000)) {
        ALOGE("NAS: poll: service not up");
        goto out;
    }
    while (qmi_client_is_up(&g_nas)) {
        int reg = 0, rif = 0, mcc = 0, mnc = 0;

        nas_refresh_sync();
        nas_get_serving(&reg, NULL, &rif, &mcc, &mnc, NULL);
        if (reg != last_reg || rif != last_rif || mcc != last_mcc ||
            mnc != last_mnc) {
            last_reg = reg;
            last_rif = rif;
            last_mcc = mcc;
            last_mnc = mnc;
            kebab_unsol(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
        }
        nas_emit_signal();
        nanosleep(&tick, NULL);
    }
out:
    pthread_mutex_lock(&s_poll_mu);
    s_poll_running = false;
    pthread_mutex_unlock(&s_poll_mu);
    return NULL;
}

static void nas_start_poll(void)
{
    pthread_t t;

    pthread_mutex_lock(&s_poll_mu);
    if (s_poll_running) {
        pthread_mutex_unlock(&s_poll_mu);
        return;
    }
    s_poll_running = true;
    pthread_mutex_unlock(&s_poll_mu);
    if (pthread_create(&t, NULL, nas_poll_thread, NULL) != 0) {
        pthread_mutex_lock(&s_poll_mu);
        s_poll_running = false;
        pthread_mutex_unlock(&s_poll_mu);
        ALOGE("NAS: cannot start poll thread");
        return;
    }
    pthread_detach(t);
}

void nas_refresh(void)
{
    pthread_t t;

    if (s_refreshing)
        return;
    s_refreshing = true;
    if (pthread_create(&t, NULL, nas_refresh_thread, NULL) != 0) {
        s_refreshing = false;
        ALOGE("NAS: cannot start refresh thread");
        return;
    }
    pthread_detach(t);
}

static void nas_ind_cb(struct qmi_client *c, uint16_t msg,
                       const uint8_t *tlvs, size_t len)
{
    (void)c;
    if (msg == NAS_GET_SERVING_SYSTEM) {
        nas_parse_serving(tlvs, len);
        nas_refresh();
    }
}

static volatile bool s_reg_running;
static pthread_mutex_t s_reg_mu = PTHREAD_MUTEX_INITIALIZER;

static void *nas_register_thread(void *arg)
{
    /* Masks captured from stock after airplane recovery. */
    static const uint8_t events[] = {
        0x10, 0x13, 0x14, 0x15, 0x17, 0x18, 0x19, 0x1a,
        0x1e, 0x1f, 0x22, 0x24, 0x27, 0x30, 0x38
    };

    (void)arg;
    for (int i = 0; i < 10 && qmi_client_is_up(&g_nas); i++) {
        uint8_t req[80];
        size_t n = qmi_put_u8(req, 0, 0x12, 1);
        int rc1, rc2;

        rc1 = qmi_transact(&g_nas, NAS_SET_EVENT_REPORT, req, n, NULL, NULL,
                           5000);
        n = 0;
        for (size_t k = 0; k < sizeof(events); k++)
            n = qmi_put_u8(req, n, events[k], 1);
        rc2 = qmi_transact(&g_nas, NAS_INDICATION_REGISTER, req, n, NULL, NULL,
                           5000);
        if (rc1 == 0 && rc2 == 0)
            break;
        sleep(1);
    }
    pthread_mutex_lock(&s_reg_mu);
    s_reg_running = false;
    pthread_mutex_unlock(&s_reg_mu);
    return NULL;
}

void nas_register_indications(void)
{
    pthread_t t;

    pthread_mutex_lock(&s_reg_mu);
    if (s_reg_running) {
        pthread_mutex_unlock(&s_reg_mu);
        return;
    }
    s_reg_running = true;
    pthread_mutex_unlock(&s_reg_mu);
    if (pthread_create(&t, NULL, nas_register_thread, NULL) != 0) {
        pthread_mutex_lock(&s_reg_mu);
        s_reg_running = false;
        pthread_mutex_unlock(&s_reg_mu);
        return;
    }
    pthread_detach(t);
}

void nas_clear_state(void)
{
    pthread_mutex_lock(&g_state.mu);
    g_state.serving_ok = false;
    g_state.reg_state = 1;
    g_state.ps_state = 0;
    g_state.radio_if = 0;
    g_state.mcc = g_state.mnc = g_state.mnc_len = 0;
    g_state.op_name[0] = '\0';
    g_state.op_short[0] = '\0';
    g_state.eons_ok = false;
    g_state.signal_ok = false;
    g_state.rssi = g_state.rsrp = g_state.rsrq = g_state.snr = 0;
    g_state.roaming = false;
    pthread_mutex_unlock(&g_state.mu);
}

static void *nas_sys_sel_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 15 && qmi_client_is_up(&g_nas); i++) {
        if (nas_apply_system_selection() == 0)
            return NULL;
        sleep(1);
    }
    ALOGE("NAS: system selection not applied");
    return NULL;
}

void nas_apply_system_selection_retry(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, nas_sys_sel_thread, NULL) == 0)
        pthread_detach(t);
}

static void nas_up_cb(struct qmi_client *c, bool up)
{
    bool radio_on;

    (void)c;
    if (up) {
        pthread_mutex_lock(&g_state.mu);
        radio_on = g_state.radio_on;
        pthread_mutex_unlock(&g_state.mu);
        nas_register_indications();
        if (radio_on)
            nas_apply_system_selection_retry();
        nas_refresh();
        nas_start_poll();
    }
}

void nas_init(void)
{
    if (qmi_client_start(&g_nas, QMI_SVC_NAS, nas_ind_cb, nas_up_cb) != 0)
        ALOGE("NAS: client start failed");
}

bool nas_get_serving(int *reg_state, int *ps_state, int *radio_if,
                     int *mcc, int *mnc, int *mnc_len)
{
    bool ok;

    pthread_mutex_lock(&g_state.mu);
    ok = g_state.serving_ok;
    if (reg_state)
        *reg_state = g_state.reg_state;
    if (ps_state)
        *ps_state = g_state.ps_state;
    if (radio_if)
        *radio_if = g_state.radio_if;
    if (mcc)
        *mcc = g_state.mcc;
    if (mnc)
        *mnc = g_state.mnc;
    if (mnc_len)
        *mnc_len = g_state.mnc_len;
    pthread_mutex_unlock(&g_state.mu);
    return ok;
}

void nas_get_op_name(char *out, size_t max)
{
    if (!out || !max)
        return;
    pthread_mutex_lock(&g_state.mu);
    strncpy(out, g_state.op_name, max - 1);
    out[max - 1] = '\0';
    pthread_mutex_unlock(&g_state.mu);
}

void nas_get_op_short(char *out, size_t max)
{
    if (!out || !max)
        return;
    pthread_mutex_lock(&g_state.mu);
    strncpy(out, g_state.op_short, max - 1);
    out[max - 1] = '\0';
    pthread_mutex_unlock(&g_state.mu);
}

bool nas_get_signal(int *rssi, int *rsrp, int *rsrq, int *snr)
{
    bool ok;

    pthread_mutex_lock(&g_state.mu);
    ok = g_state.signal_ok;
    if (rssi)
        *rssi = g_state.rssi;
    if (rsrp)
        *rsrp = g_state.rsrp;
    if (rsrq)
        *rsrq = g_state.rsrq;
    if (snr)
        *snr = g_state.snr;
    pthread_mutex_unlock(&g_state.mu);
    return ok;
}

int nas_apply_system_selection(void)
{
    uint8_t req[64];
    size_t n = 0;
    uint16_t mode = 0x0050; /* LTE | NR5G */
    uint8_t sel = 0;        /* automatic */
    uint32_t domain = 2;    /* PS only (Jio); override with the property */
    uint8_t duration = 1;   /* immediate */
    char prop[PROPERTY_VALUE_MAX];

    if (property_get("persist.vendor.kebab.service_domain", prop, "2") > 0) {
        int v = atoi(prop);

        if (v >= 1 && v <= 3)
            domain = (uint32_t)v;
    }

    if (!qmi_client_is_up(&g_nas))
        return -ENOTCONN;

    n = qmi_put_u16(req, n, 0x11, mode);
    n = qmi_put_u8(req, n, 0x10, sel);
    n = qmi_put_u32(req, n, 0x12, domain);
    n = qmi_put_u8(req, n, 0x17, duration);
    return qmi_transact(&g_nas, NAS_SET_SYS_SEL_PREF, req, n, NULL, NULL, 10000);
}
