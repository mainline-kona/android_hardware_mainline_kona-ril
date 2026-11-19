/*
 * uim.c — SIM presence, ICCID and IMSI over QMI UIM.
 *
 * Wire layouts are taken from the in-tree libqmi IDL (qmi-service-uim.json):
 *   0x2F Get Card Status  : no input; output TLV 0x10 sequence
 *                           {u16 x4 indexes, u8 n_cards, cards[]}
 *                           card = {u8 card_state, u8 upin, u8 upin_r,
 *                                   u8 upuk_r, u8 err, u8 n_apps, apps[]}
 *   0x20 Read Transparent : TLV 0x01 {u8 session, u8 aid_len, aid[]}
 *                           TLV 0x02 {u16 file_id, u8 path_len, path[]}
 *                           TLV 0x03 {u16 offset, u16 length}
 *                           output TLV 0x11 {u16 len, bytes[]}
 * Card state 1 = present. Slot session type 6 = card slot 1 (stock value).
 */
#define LOG_TAG "kebab-rild"

#include "uim.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#define UIM_GET_CARD_STATUS   0x002F
#define UIM_READ_TRANSPARENT  0x0020
#define UIM_READ_RECORD       0x0021
#define UIM_REGISTER_EVENTS   0x002E
#define UIM_SEND_APDU         0x003B
#define UIM_OPEN_CHANNEL      0x0042
#define UIM_SLOT1             1
#define UIM_SESSION_PRI_GW_PROV 0
#define UIM_SESSION_CARD_SLOT1  6

#define EF_ICCID  0x2FE2
#define EF_IMSI   0x6F07
#define EF_SPN    0x6F46
#define EF_MSISDN 0x6F40

static volatile bool s_setup_running;
static pthread_mutex_t s_setup_mu = PTHREAD_MUTEX_INITIALIZER;

static void bcd_to_digits(const uint8_t *bcd, size_t len, char *out,
                          size_t out_max)
{
    size_t o = 0;

    for (size_t i = 0; i < len && o + 1 < out_max; i++) {
        uint8_t lo = bcd[i] & 0x0f;
        uint8_t hi = bcd[i] >> 4;

        if (lo == 0x0f)
            break;
        out[o++] = (char)('0' + lo);
        if (hi == 0x0f || o + 1 >= out_max)
            break;
        out[o++] = (char)('0' + hi);
    }
    out[o] = '\0';
}

static void uim_parse_msisdn(const uint8_t *raw, size_t len, char *out,
                             size_t out_max)
{
    size_t n, o = 0;

    if (!out || !out_max)
        return;
    out[0] = '\0';
    if (!raw || len < 3)
        return;
    n = raw[0]; /* bytes following the length: TON/NPI + BCD digits */
    if (n == 0 || n + 1 > len)
        n = len - 1;
    for (size_t i = 2; i < 1 + n && i < len && o + 1 < out_max; i++) {
        uint8_t lo = raw[i] & 0x0f;
        uint8_t hi = raw[i] >> 4;

        if (lo != 0x0f)
            out[o++] = (char)('0' + lo);
        if (hi != 0x0f && o + 1 < out_max)
            out[o++] = (char)('0' + hi);
    }
    out[o] = '\0';
}

static void uim_parse_imsi(const uint8_t *raw, size_t len, char *out,
                           size_t out_max)
{
    size_t imsi_len, o = 0;

    if (!out || !out_max)
        return;
    out[0] = '\0';
    if (!raw || len < 2)
        return;

    /* TS 31.102: raw[0] counts the following octets; the low nibble of
     * raw[1] is the odd/even indicator and is skipped. */
    imsi_len = raw[0];
    if (imsi_len == 0 || imsi_len > len - 1)
        imsi_len = len - 1;
    if (imsi_len == 0 || (raw[1] >> 4) >= 10)
        return;

    out[o++] = (char)('0' + (raw[1] >> 4));
    for (size_t i = 2; i <= imsi_len && o + 1 < out_max; i++) {
        uint8_t lo = raw[i] & 0x0f;
        uint8_t hi = raw[i] >> 4;

        if (lo < 10 && o + 1 < out_max)
            out[o++] = (char)('0' + lo);
        if (hi < 10 && o + 1 < out_max)
            out[o++] = (char)('0' + hi);
    }
    out[o] = '\0';
    if (o < 5)
        out[0] = '\0';
}

static int uim_read_ef(uint8_t session_type, uint16_t file_id,
                       const uint8_t *path, uint8_t path_len,
                       uint8_t *out, size_t out_max)
{
    uint8_t req[64];
    uint8_t sess[2] = { session_type, 0 };
    uint8_t file[3 + 10];
    uint8_t ri[4];
    uint8_t rsp[512];
    size_t n = 0, rl = sizeof(rsp);
    int rc;
    uint16_t l = 0;
    const uint8_t *rr;
    uint16_t dl;

    if (path_len > 10)
        return -EINVAL;

    n = qmi_put_tlv(req, n, 0x01, sess, sizeof(sess));
    file[0] = (uint8_t)(file_id & 0xff);
    file[1] = (uint8_t)(file_id >> 8);
    file[2] = path_len;
    if (path_len && path)
        memcpy(file + 3, path, path_len);
    n = qmi_put_tlv(req, n, 0x02, file, (uint16_t)(3 + path_len));
    ri[0] = 0; /* offset */
    ri[1] = 0;
    ri[2] = (uint8_t)(out_max & 0xff);
    ri[3] = (uint8_t)(out_max >> 8);
    n = qmi_put_tlv(req, n, 0x03, ri, sizeof(ri));

    rc = qmi_transact(&g_uim, UIM_READ_TRANSPARENT, req, n, rsp, &rl, 15000);
    if (rc) {
        ALOGE("UIM read 0x%04x: error %d", file_id, rc);
        return rc < 0 ? rc : -EIO;
    }
    rr = qmi_find_tlv(rsp, rl, 0x11, &l);
    if (!rr || l < 2) {
        ALOGE("UIM read 0x%04x: no read result", file_id);
        return -ENODATA;
    }
    dl = (uint16_t)(rr[0] | (rr[1] << 8));
    if (dl > (uint16_t)(l - 2))
        dl = (uint16_t)(l - 2);
    if (dl > out_max)
        dl = (uint16_t)out_max;
    memcpy(out, rr + 2, dl);
    return dl;
}

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode(const char *in, uint8_t *out, size_t max)
{
    int val = 0, bits = -8;
    size_t n = 0;

    for (; in && *in; in++) {
        const char *p;

        if (*in == '=' || *in == '\n' || *in == '\r')
            continue;
        p = strchr(b64tab, *in);
        if (!p)
            return -EINVAL;
        val = (val << 6) | (int)(p - b64tab);
        bits += 6;
        if (bits >= 0) {
            if (n >= max)
                return -E2BIG;
            out[n++] = (uint8_t)((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    return (int)n;
}

static void b64_encode(const uint8_t *in, size_t len, char *out, size_t max)
{
    size_t i, o = 0;

    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                     in[i + 2];

        if (o + 4 >= max)
            break;
        out[o++] = b64tab[(v >> 18) & 0x3f];
        out[o++] = b64tab[(v >> 12) & 0x3f];
        out[o++] = b64tab[(v >> 6) & 0x3f];
        out[o++] = b64tab[v & 0x3f];
    }
    if (i < len && o + 4 < max) {
        uint32_t v = (uint32_t)in[i] << 16;

        if (i + 1 < len)
            v |= (uint32_t)in[i + 1] << 8;
        out[o++] = b64tab[(v >> 18) & 0x3f];
        out[o++] = b64tab[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < len) ? b64tab[(v >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }
    out[o < max ? o : max - 1] = '\0';
}

static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;

    while (hex && hex[0] && hex[1] && n < max) {
        unsigned v;

        if (sscanf(hex, "%2x", &v) != 1)
            return -EINVAL;
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return (int)n;
}

static int uim_open_channel(const uint8_t *aid, size_t alen)
{
    uint8_t req[80], rsp[256];
    size_t n = 0, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;

    n = qmi_put_u8(req, n, 0x01, UIM_SLOT1);
    n = qmi_put_tlv(req, n, 0x10, aid, (uint16_t)alen);
    if (qmi_transact(&g_uim, UIM_OPEN_CHANNEL, req, n, rsp, &rl, 10000))
        return -EIO;
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (!v || l < 1)
        return -ENODATA;
    return v[0];
}

static int uim_send_apdu(uint8_t channel, bool have_chan,
                         const uint8_t *apdu, size_t alen,
                         uint8_t *out, size_t out_max)
{
    uint8_t req[512], rsp[768];
    uint8_t arr[2 + 256];
    size_t n = 0, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;
    uint16_t dl;

    if (alen > 256)
        return -E2BIG;
    arr[0] = (uint8_t)(alen & 0xff);
    arr[1] = (uint8_t)(alen >> 8);
    memcpy(arr + 2, apdu, alen);
    n = qmi_put_u8(req, n, 0x01, UIM_SLOT1);
    n = qmi_put_tlv(req, n, 0x02, arr, (uint16_t)(2 + alen));
    if (have_chan)
        n = qmi_put_u8(req, n, 0x10, channel);
    if (qmi_transact(&g_uim, UIM_SEND_APDU, req, n, rsp, &rl, 15000))
        return -EIO;
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (!v || l < 2)
        return -ENODATA;
    dl = (uint16_t)(v[0] | (v[1] << 8));
    if (dl > (uint16_t)(l - 2))
        dl = (uint16_t)(l - 2);
    if (dl > out_max)
        dl = (uint16_t)out_max;
    memcpy(out, v + 2, dl);
    return dl;
}

int uim_sim_auth(int auth_context, const char *auth_data_b64, const char *aid,
                 char *out_b64, size_t out_max, int *sw1, int *sw2)
{
    uint8_t challenge[128];
    uint8_t apdu[160];
    uint8_t aidb[64];
    uint8_t rsp[256];
    static char cached_aid[130];
    static int cached_chan = -1;
    int clen, aidl, dl, chan = -1;
    size_t alen;

    if (!auth_data_b64 || !out_b64 || out_max < 8)
        return -EINVAL;
    clen = b64_decode(auth_data_b64, challenge, sizeof(challenge));
    if (clen < 16)
        return -EINVAL;
    alen = 5 + (size_t)clen;
    if (alen > sizeof(apdu))
        return -E2BIG;
    apdu[0] = 0x88; /* AUTHENTICATE */
    apdu[1] = 0x88;
    apdu[2] = 0x00;
    apdu[3] = (uint8_t)auth_context;
    apdu[4] = (uint8_t)clen;
    memcpy(apdu + 5, challenge, (size_t)clen);

    aidl = (aid && aid[0]) ? hex_decode(aid, aidb, sizeof(aidb)) : 0;
    if (aidl > 0) {
        if (!strcmp(cached_aid, aid) && cached_chan >= 0) {
            chan = cached_chan;
        } else {
            chan = uim_open_channel(aidb, (size_t)aidl);
            if (chan >= 0) {
                snprintf(cached_aid, sizeof(cached_aid), "%s", aid);
                cached_chan = chan;
            }
        }
    }

    dl = uim_send_apdu(chan >= 0 ? (uint8_t)chan : 0, chan >= 0, apdu, alen,
                       rsp, sizeof(rsp));
    if (dl < 2)
        return dl < 0 ? dl : -ENODATA;
    if (sw1)
        *sw1 = rsp[dl - 2];
    if (sw2)
        *sw2 = rsp[dl - 1];
    dl -= 2;
    if (rsp[dl] != 0x90 || rsp[dl + 1] != 0x00)
        return -EIO;
    b64_encode(rsp, (size_t)dl, out_b64, out_max);
    return 0;
}

static void *uim_setup_thread(void *arg)
{
    static const uint8_t p_iccid[] = { 0x00, 0x3f };
    static const uint8_t p_imsi[] = { 0x00, 0x3f, 0xff, 0x7f };
    uint8_t rsp[1024];
    uint8_t bcd[16];
    size_t rl = sizeof(rsp);
    int card = 3;
    int rc, n;

    (void)arg;
    if (qmi_client_wait_up(&g_uim, 20000)) {
        ALOGE("UIM: service not up");
        goto out;
    }

    /* Card/refresh/slot events, same mask the stock RIL registers. */
    {
        uint8_t req[8];
        size_t n = qmi_put_u32(req, 0, 0x01, 0x987u);

        rc = qmi_transact(&g_uim, UIM_REGISTER_EVENTS, req, n, NULL, NULL, 5000);
        if (rc)
            ALOGW("UIM: event register failed %d", rc);
    }

    /* After an SSR the UIM port is announced before the card controller is
     * ready; retry instead of caching card_state = unknown forever. */
    for (int attempt = 0; attempt < 10; attempt++) {
        rl = sizeof(rsp);
        rc = qmi_transact(&g_uim, UIM_GET_CARD_STATUS, NULL, 0, rsp, &rl, 20000);
        if (rc == 0)
            break;
        ALOGE("UIM: get card status error %d (attempt %d)", rc, attempt + 1);
        sleep(1);
        if (!qmi_client_is_up(&g_uim))
            break;
    }
    if (rc == 0) {
        uint16_t l = 0;
        const uint8_t *cs17 = qmi_find_tlv(rsp, rl, 0x17, &l);
        const uint8_t *cs10 = qmi_find_tlv(rsp, rl, 0x10, &l);

        if (cs17 && l >= 12 && cs17[1] >= 1) {
            /* SDX55 TLV 0x17: extended card status. cs17[1] = iccid_len (10), cs17[2..] = BCD ICCID */
            card = 1;
            uint8_t iclen = cs17[1] > 10 ? 10 : cs17[1];
            if (iclen > l - 2)
                iclen = (uint8_t)(l - 2);
            char icstr[24];

            bcd_to_digits(cs17 + 2, iclen, icstr, sizeof(icstr));
            pthread_mutex_lock(&g_state.mu);
            strncpy(g_state.iccid, icstr, sizeof(g_state.iccid) - 1);
            g_state.iccid_ok = true;
            pthread_mutex_unlock(&g_state.mu);
            ALOGI("UIM: ICCID from TLV 0x17: %s", icstr);
        } else if (cs10 && l >= 10 && cs10[8] >= 1) {
            card = cs10[9]; /* 1 = present */
        } else {
            card = 1;     /* modem accepted the query */
        }
    } else {
        ALOGE("UIM: get card status error %d", rc);
    }

    pthread_mutex_lock(&g_state.mu);
    g_state.card_state = card;
    pthread_mutex_unlock(&g_state.mu);
    ALOGI("UIM: card state %d", card);

    if (card != 1) {
        kebab_unsol(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        goto out;
    }

    /* If ICCID was not populated from TLV 0x17, try reading EF_ICCID directly */
    if (!g_state.iccid_ok) {
        n = uim_read_ef(UIM_SESSION_CARD_SLOT1, EF_ICCID, p_iccid,
                        sizeof(p_iccid), bcd, 10);
        if (n > 0) {
            char tmp[24];

            bcd_to_digits(bcd, (size_t)n, tmp, sizeof(tmp));
            pthread_mutex_lock(&g_state.mu);
            strncpy(g_state.iccid, tmp, sizeof(g_state.iccid) - 1);
            g_state.iccid_ok = true;
            pthread_mutex_unlock(&g_state.mu);
            ALOGI("UIM: ICCID %s", tmp);
        }
    }

    /* EF_IMSI lives under the current ADF (3F00/7FFF) on session 0. */
    n = uim_read_ef(UIM_SESSION_PRI_GW_PROV, EF_IMSI, p_imsi,
                    sizeof(p_imsi), bcd, 9);
    if (n > 0) {
        char tmp[20];

        uim_parse_imsi(bcd, (size_t)n, tmp, sizeof(tmp));
        if (tmp[0]) {
            pthread_mutex_lock(&g_state.mu);
            strncpy(g_state.imsi, tmp, sizeof(g_state.imsi) - 1);
            g_state.imsi_ok = true;
            pthread_mutex_unlock(&g_state.mu);
            ALOGI("UIM: IMSI %s", tmp);
        }
    }

    /* EF_SPN: byte 0 is the display condition, the name follows until 0xFF. */
    {
        static const uint8_t p_spn[] = { 0x00, 0x3f, 0xff, 0x7f };
        uint8_t spn_raw[24];

        n = uim_read_ef(UIM_SESSION_PRI_GW_PROV, EF_SPN, p_spn,
                        sizeof(p_spn), spn_raw, sizeof(spn_raw));
        if (n > 1) {
            char tmp[24];
            size_t o = 0;

            for (int i = 1; i < n && o + 1 < sizeof(tmp); i++) {
                if (spn_raw[i] == 0xff)
                    break;
                tmp[o++] = (char)spn_raw[i];
            }
            tmp[o] = '\0';
            if (o) {
                pthread_mutex_lock(&g_state.mu);
                strncpy(g_state.spn, tmp, sizeof(g_state.spn) - 1);
                g_state.spn_ok = true;
                pthread_mutex_unlock(&g_state.mu);
                ALOGI("UIM: SPN %s", tmp);
            }
        }
    }
    /* EF_MSISDN: under the USIM ADF, or DF_TELECOM on older cards. */
    {
        static const uint8_t p_adf[] = { 0x00, 0x3f, 0xff, 0x7f };
        static const uint8_t p_tel[] = { 0x00, 0x3f, 0x10, 0x7f };
        uint8_t mraw[32];
        char num[24];

        n = uim_read_ef(UIM_SESSION_PRI_GW_PROV, EF_MSISDN, p_adf,
                        sizeof(p_adf), mraw, sizeof(mraw));
        if (n <= 1)
            n = uim_read_ef(UIM_SESSION_PRI_GW_PROV, EF_MSISDN, p_tel,
                            sizeof(p_tel), mraw, sizeof(mraw));
        if (n > 1) {
            uim_parse_msisdn(mraw, (size_t)n, num, sizeof(num));
            if (num[0]) {
                pthread_mutex_lock(&g_state.mu);
                snprintf(g_state.msisdn, sizeof(g_state.msisdn), "%s", num);
                g_state.msisdn_ok = true;
                pthread_mutex_unlock(&g_state.mu);
                ALOGI("UIM: MSISDN %s", num);
            }
        }
    }

    kebab_unsol(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
out:
    state_update_readiness();
    pthread_mutex_lock(&s_setup_mu);
    s_setup_running = false;
    pthread_mutex_unlock(&s_setup_mu);
    return NULL;
}

static void uim_spawn_setup(void)
{
    pthread_t t;

    pthread_mutex_lock(&s_setup_mu);
    if (s_setup_running) {
        pthread_mutex_unlock(&s_setup_mu);
        return;
    }
    s_setup_running = true;
    pthread_mutex_unlock(&s_setup_mu);
    if (pthread_create(&t, NULL, uim_setup_thread, NULL) != 0) {
        pthread_mutex_lock(&s_setup_mu);
        s_setup_running = false;
        pthread_mutex_unlock(&s_setup_mu);
        ALOGE("UIM: cannot start setup thread");
        return;
    }
    pthread_detach(t);
}

static volatile bool s_card_running;
static pthread_mutex_t s_card_mu = PTHREAD_MUTEX_INITIALIZER;

static int card_state_from_rsp(const uint8_t *rsp, size_t rl)
{
    uint16_t l = 0;
    const uint8_t *v = qmi_find_tlv(rsp, rl, 0x17, &l);

    if (v && l >= 12)
        return 1;
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (v && l >= 10 && v[8] >= 1)
        return v[9];
    return 1;
}

static void *uim_card_thread(void *arg)
{
    uint8_t rsp[1024];
    size_t rl = sizeof(rsp);
    int rc = -1, card = 3, old;

    (void)arg;
    for (int i = 0; i < 5 && qmi_client_is_up(&g_uim); i++) {
        rl = sizeof(rsp);
        rc = qmi_transact(&g_uim, UIM_GET_CARD_STATUS, NULL, 0, rsp, &rl,
                          10000);
        if (rc == 0)
            break;
        sleep(1);
    }
    if (rc == 0)
        card = card_state_from_rsp(rsp, rl);

    pthread_mutex_lock(&g_state.mu);
    old = g_state.card_state;
    if (rc == 0)
        g_state.card_state = card;
    pthread_mutex_unlock(&g_state.mu);

    if (rc == 0 && card != old) {
        kebab_unsol(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        state_update_readiness();
    }
    pthread_mutex_lock(&s_card_mu);
    s_card_running = false;
    pthread_mutex_unlock(&s_card_mu);
    return NULL;
}

void uim_poll_card(void)
{
    pthread_t t;
    bool busy;

    pthread_mutex_lock(&s_setup_mu);
    busy = s_setup_running;
    pthread_mutex_unlock(&s_setup_mu);
    if (busy)
        return;
    pthread_mutex_lock(&s_card_mu);
    if (s_card_running) {
        pthread_mutex_unlock(&s_card_mu);
        return;
    }
    s_card_running = true;
    pthread_mutex_unlock(&s_card_mu);
    if (pthread_create(&t, NULL, uim_card_thread, NULL) != 0) {
        pthread_mutex_lock(&s_card_mu);
        s_card_running = false;
        pthread_mutex_unlock(&s_card_mu);
        return;
    }
    pthread_detach(t);
}

static void uim_ind_cb(struct qmi_client *c, uint16_t msg,
                       const uint8_t *tlvs, size_t len)
{
    (void)c;
    (void)tlvs;
    (void)len;
    ALOGI("UIM: indication 0x%04x", msg);
    /* Card status (0x32), refresh (0x33) and slot status (0x48) all mean the
     * cached identity may be stale. */
    if (msg == 0x0032 || msg == 0x0033 || msg == 0x0048 ||
        msg == UIM_GET_CARD_STATUS) {
        uim_spawn_setup();
        kebab_unsol(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
    }
}

static void uim_up_cb(struct qmi_client *c, bool up)
{
    (void)c;
    if (up)
        uim_spawn_setup();
}

void uim_init(void)
{
    if (qmi_client_start(&g_uim, QMI_SVC_UIM, uim_ind_cb, uim_up_cb) != 0)
        ALOGE("UIM: client start failed");
}

bool uim_ready(void)
{
    bool ok;

    pthread_mutex_lock(&g_state.mu);
    ok = g_state.card_state == 1 && g_state.iccid_ok;
    pthread_mutex_unlock(&g_state.mu);
    return ok;
}

void uim_refresh(void)
{
    uim_spawn_setup();
}

void uim_get_msisdn(char *out, size_t max)
{
    if (!out || !max)
        return;
    pthread_mutex_lock(&g_state.mu);
    snprintf(out, max, "%s", g_state.msisdn_ok ? g_state.msisdn : "");
    pthread_mutex_unlock(&g_state.mu);
}

void uim_get_spn(char *out, size_t max)
{
    if (!out || !max)
        return;
    pthread_mutex_lock(&g_state.mu);
    strncpy(out, g_state.spn, max - 1);
    out[max - 1] = '\0';
    pthread_mutex_unlock(&g_state.mu);
}

int uim_sim_io(int command, int fileid, const char *path, const char *aid_hex,
               int p1, int p2, int p3, int *sw1, int *sw2, char *hex,
               size_t hex_max)
{
    uint8_t req[128], rsp[600], pb[10];
    uint8_t sess[2 + 64] = { UIM_SESSION_PRI_GW_PROV, 0 };
    size_t sess_len = 2;
    uint8_t file[3 + sizeof(pb)];
    uint8_t ri[4];
    size_t n = 0, rl = sizeof(rsp), pl = 0, hl = 0;
    uint16_t off, rlen, l = 0, dl;
    const char *h = path;
    const uint8_t *v;
    int rc;

    bool rec = command == 0xB2;
    uint16_t msgid;

    if (command != 0xB0 && !rec)
        return -EOPNOTSUPP;
    if (!hex || hex_max < 3 || !sw1 || !sw2)
        return -EINVAL;

    while (h && h[0] && h[1] && h[2] && h[3] && pl + 2 <= sizeof(pb)) {
        char b[5] = { h[0], h[1], h[2], h[3], 0 };
        unsigned val = (unsigned)strtoul(b, NULL, 16);

        if (val == 0x7F20) /* USIM ADF: the modem only resolves 7FFF (current ADF) */
            val = 0x7FFF;
        pb[pl++] = (uint8_t)(val & 0xff);
        pb[pl++] = (uint8_t)(val >> 8);
        h += 4;
    }

    if (aid_hex && aid_hex[0]) {
        int al = hex_decode(aid_hex, sess + 2, sizeof(sess) - 2);

        if (al > 0) {
            sess[1] = (uint8_t)al;
            sess_len = 2 + (size_t)al;
        }
    }
    n = qmi_put_tlv(req, n, 0x01, sess, (uint16_t)sess_len);
    file[0] = (uint8_t)(fileid & 0xff);
    file[1] = (uint8_t)(((unsigned)fileid >> 8) & 0xff);
    if (rec) {
        memcpy(file + 2, pb, pl); /* record File TLV has no path-length byte */
        n = qmi_put_tlv(req, n, 0x02, file, (uint16_t)(2 + pl));
        rlen = (uint16_t)(p3 > 0 ? p3 : 0);
        ri[0] = (uint8_t)(p1 & 0xff);
        ri[1] = (uint8_t)((p1 >> 8) & 0xff);
        ri[2] = (uint8_t)(rlen & 0xff);
        ri[3] = (uint8_t)(rlen >> 8);
        n = qmi_put_tlv(req, n, 0x03, ri, sizeof(ri));
        msgid = UIM_READ_RECORD;
    } else {
        file[2] = (uint8_t)pl;
        memcpy(file + 3, pb, pl);
        n = qmi_put_tlv(req, n, 0x02, file, (uint16_t)(3 + pl));
        off = (uint16_t)(((p1 & 0xff) << 8) | (p2 & 0xff));
        rlen = (uint16_t)(p3 > 0 ? p3 : 0);
        ri[0] = (uint8_t)(off & 0xff);
        ri[1] = (uint8_t)(off >> 8);
        ri[2] = (uint8_t)(rlen & 0xff);
        ri[3] = (uint8_t)(rlen >> 8);
        n = qmi_put_tlv(req, n, 0x03, ri, sizeof(ri));
        msgid = UIM_READ_TRANSPARENT;
    }

    rc = qmi_transact(&g_uim, msgid, req, n, rsp, &rl, 10000);
    if (rc)
        return rc < 0 ? rc : -EIO;
    v = qmi_find_tlv(rsp, rl, 0x11, &l);
    if (!v || l < 2)
        return -ENODATA;
    dl = (uint16_t)(v[0] | (v[1] << 8));
    if (dl > (uint16_t)(l - 2))
        dl = (uint16_t)(l - 2);
    for (uint16_t i = 0; i < dl && hl + 2 < hex_max; i++) {
        static const char hx[] = "0123456789abcdef";

        hex[hl++] = hx[v[2 + i] >> 4];
        hex[hl++] = hx[v[2 + i] & 0x0f];
    }
    hex[hl] = '\0';

    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (v && l >= 2) {
        *sw1 = v[0];
        *sw2 = v[1];
    } else {
        *sw1 = 0x6f;
        *sw2 = 0;
    }
    return 0;
}
