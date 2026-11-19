/*
 * kebab-rild-mock-test.c — host test with synthetic QMI replies.
 *
 * Overrides the qmi_test_* seams so the daemon's real modules run against
 * canned modem answers: SIM identity, registration, signal, SMS send, voice
 * dial/call list, data setup and cell info are asserted end to end.
 */
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "guest/hals/ril/reference-libril/ril.h"
#include "qmi/qmi.h"
#include "ims.h"
#include "nas.h"
#include "state.h"
#include "uim.h"

#define REG_STATE_LEN 18

static int failures;

static pthread_mutex_t done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
static int done_flag;
static int mock_flags;
static int mock_auth_apdu_ok;

#define MOCK_SMS_ERR70   0x01
#define MOCK_SMS_RP38    0x02
#define MOCK_IMS_UNREG   0x04
#define MOCK_UIM_ABSENT  0x08
#define MOCK_WDS_FAIL    0x10
#define MOCK_DIAL_FAIL   0x20
#define MOCK_USSD_GSM7   0x40
#define MOCK_SIG_NR      0x80
#define MOCK_EONS_LTE    0x100
#define MOCK_SIG_LTE_NR  0x200

#define CHECK(cond, what) do { \
    if (cond) printf("PASS %s\n", what); \
    else { printf("FAIL %s\n", what); failures++; } \
} while (0)

struct snapshot {
    RIL_Errno err;
    size_t len;
    uint8_t data[2048];
    char *strs[24];
    int nstr;
};

static struct snapshot snap;
static int expect_strings;
static void (*on_resp_verify)(void);

/* Last request seen by the mock and last unsolicited delivered. */
static struct {
    uint16_t svc, msg;
    uint8_t tlvs[256];
    size_t len;
} last_req;

static struct {
    int id;
    int count;
    uint8_t data[512];
    size_t len;
    char *strs[2];
} last_unsol;

static void snap_clear(void)
{
    for (int i = 0; i < snap.nstr; i++)
        free(snap.strs[i]);
    memset(&snap, 0, sizeof(snap));
}

static void on_complete(RIL_Token t, RIL_Errno e, void *resp, size_t len)
{
    (void)t;
    snap.err = e;
    snap.len = len;
    if (resp && len <= sizeof(snap.data))
        memcpy(snap.data, resp, len);
    if (resp && expect_strings > 0) {
        char **v = resp;

        for (int i = 0; i < expect_strings && i < 24; i++)
            snap.strs[snap.nstr++] = strdup(v[i] ? v[i] : "");
    }
    expect_strings = 0;
    /* Pointer-bearing responses (call lists) are only valid during the
     * completion: inspect them here, as liblibril does. */
    if (on_resp_verify) {
        void (*fn)(void) = on_resp_verify;

        on_resp_verify = NULL;
        fn();
    }
    pthread_mutex_lock(&done_mu);
    done_flag = 1;
    pthread_cond_broadcast(&done_cv);
    pthread_mutex_unlock(&done_mu);
}

static void on_unsol(int response, const void *data, size_t len)
{
    last_unsol.count++;
    last_unsol.id = response;
    last_unsol.len = len;
    memset(last_unsol.data, 0, sizeof(last_unsol.data));
    if (data && len < sizeof(last_unsol.data))
        memcpy(last_unsol.data, data, len);
    for (int i = 0; i < 2; i++)
        free(last_unsol.strs[i]);
    last_unsol.strs[0] = last_unsol.strs[1] = NULL;
    if (response == RIL_UNSOL_ON_USSD && data && len >= 2 * sizeof(char *)) {
        char **v = (char **)data;

        last_unsol.strs[0] = strdup(v[0] ? v[0] : "");
        last_unsol.strs[1] = strdup(v[1] ? v[1] : "");
    }
}

static void timed_cb(RIL_TimedCallback cb, void *param,
                     const struct timeval *tv)
{
    (void)cb;
    (void)param;
    (void)tv;
}

/* ---------------- mock QMI ---------------- */

bool qmi_test_is_up(struct qmi_client *c)
{
    (void)c;
    return true;
}

static size_t put_result_ok(uint8_t *rsp)
{
    (void)rsp;
    return 0; /* qmi_transact returns rc directly; no result TLV needed */
}

static const uint8_t ICCID_BCD[10] = {
    0x98, 0x19, 0x68, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t IMSI_BCD[9] = {
    0x08, 0x40, 0x50, 0x68, 0x01, 0x00, 0x00, 0x00, 0x10
};

int qmi_test_transact(struct qmi_client *c, uint16_t msgid,
                      const uint8_t *req, size_t reqlen,
                      uint8_t *rsp, size_t rsp_cap,
                      size_t *rsp_len)
{
    size_t n = 0;
    uint16_t l = 0;
    const uint8_t *v;

    (void)reqlen;
    (void)rsp_cap;

    last_req.svc = c->service;
    last_req.msg = msgid;
    last_req.len = reqlen < sizeof(last_req.tlvs) ? reqlen : sizeof(last_req.tlvs);
    if (req && last_req.len)
        memcpy(last_req.tlvs, req, last_req.len);

    if (!rsp || !rsp_len)
        return 0;
    *rsp_len = 0;

    switch (c->service) {
    case QMI_SVC_UIM:
        if (msgid == 0x002F) { /* Get Card Status: one present card */
            uint8_t cs[15] = { 0 };

            cs[8] = 1;  /* one card */
            cs[9] = (mock_flags & MOCK_UIM_ABSENT) ? 0 : 1;
            n = qmi_put_tlv(rsp, 0, 0x10, cs, sizeof(cs));
            *rsp_len = n;
        } else if (msgid == 0x003B) { /* Send APDU: 4-byte AKA payload + 9000 */
            uint8_t ap[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0x90, 0x00 };
            uint8_t buf[8];
            uint16_t al = 0;
            const uint8_t *av = qmi_find_tlv(req, reqlen, 0x02, &al);

            if (av && al >= 7 && av[0] == 0x25 && av[1] == 0x00 &&
                av[2] == 0x88 && av[3] == 0x88 && av[4] == 0x00 &&
                av[5] == 0x81 && av[6] == 0x20)
                mock_auth_apdu_ok = 1;
            buf[0] = sizeof(ap);
            buf[1] = 0;
            memcpy(buf + 2, ap, sizeof(ap));
            n = qmi_put_tlv(rsp, 0, 0x10, buf, sizeof(buf));
            *rsp_len = n;
        } else if (msgid == 0x0020) { /* Read Transparent: ICCID or IMSI */
            const uint8_t *data = ICCID_BCD;
            size_t dl = sizeof(ICCID_BCD);
            uint8_t buf[16];

            v = qmi_find_tlv(req, reqlen, 0x02, &l);
            if (v && l >= 2 && (uint16_t)(v[0] | (v[1] << 8)) == 0x6F07) {
                data = IMSI_BCD;
                dl = sizeof(IMSI_BCD);
            } else if (v && l >= 2 &&
                       (uint16_t)(v[0] | (v[1] << 8)) == 0x6F40) {
                static const uint8_t msisdn_bcd[] = {
                    0x07, 0x91, 0x19, 0x87, 0x09, 0x00, 0x00, 0x00
                };

                data = msisdn_bcd;
                dl = sizeof(msisdn_bcd);
            }
            buf[0] = (uint8_t)(dl & 0xff);
            buf[1] = (uint8_t)(dl >> 8);
            memcpy(buf + 2, data, dl);
            n = qmi_put_tlv(rsp, 0, 0x11, buf, (uint16_t)(2 + dl));
            {
                uint8_t sw[2] = { 0x90, 0x00 };

                n = qmi_put_tlv(rsp, n, 0x10, sw, sizeof(sw));
            }
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_NAS:
        if (msgid == 0x0024) { /* serving: registered LTE, 405/861 */
            uint8_t ss[6] = { 1, 1, 1, 1, 1, 8 };
            uint8_t plmn[11] = { 0x95, 0x01, 0x5d, 0x03, 0x06,
                                 'J', 'I', 'O', ' ', '4', 'G' };

            n = qmi_put_tlv(rsp, 0, 0x01, ss, sizeof(ss));
            n = qmi_put_tlv(rsp, n, 0x12, plmn, sizeof(plmn));
            *rsp_len = n;
        } else if (msgid == 0x0021) { /* network scan: two operators */
            uint8_t arr[64];
            size_t p = 0;

            /* {u16 mcc, u16 mnc, u8 status, string} x2, byte-count prefix */
            arr[0] = 0; /* patched below */
            arr[1] = 0;
            p = 2;
            memcpy(arr + p, "\x95\x01\x5d\x03", 4); p += 4;
            arr[p++] = 1;
            memcpy(arr + p, "Jio", 4); p += 4;
            memcpy(arr + p, "\x4e\x00\x0a\x00", 4); p += 4;
            arr[p++] = 1;
            memcpy(arr + p, "Airtel", 7); p += 7;
            arr[0] = (uint8_t)((p - 2) & 0xff);
            arr[1] = (uint8_t)((p - 2) >> 8);
            n = qmi_put_tlv(rsp, 0, 0x10, arr, (uint16_t)p);
            *rsp_len = n;
        } else if (msgid == 0x004F) {
            if (mock_flags & MOCK_SIG_NR) {
                /* NR: rsrp -91 dBm, snr 23.5 dB, rsrq -11 dB */
                uint8_t nr[4] = { 0xa5, 0xff, 0xeb, 0x00 };
                uint8_t rq[2] = { 0xf5, 0xff };

                n = qmi_put_tlv(rsp, 0, 0x17, nr, sizeof(nr));
                n = qmi_put_tlv(rsp, n, 0x18, rq, sizeof(rq));
            } else {
                /* LTE (stock wire): rssi -60, rsrq -11 dB, rsrp -89 dBm, snr 16.6 dB */
                uint8_t sig[6];

                sig[0] = 0xc4;
                sig[1] = 0xf5;
                sig[2] = 0xa7;
                sig[3] = 0xff;
                sig[4] = 0xa6;
                sig[5] = 0x00;
                n = qmi_put_tlv(rsp, 0, 0x14, sig, sizeof(sig));
                if (mock_flags & MOCK_SIG_LTE_NR) {
                    /* Live LTE wire also carries empty NR TLVs. */
                    uint8_t nr[4] = { 0x00, 0x80, 0x00, 0x80 };
                    uint8_t rq[2] = { 0x00, 0x80 };

                    n = qmi_put_tlv(rsp, n, 0x17, nr, sizeof(nr));
                    n = qmi_put_tlv(rsp, n, 0x18, rq, sizeof(rq));
                }
            }
            *rsp_len = n;
        } else if (msgid == 0x0044) { /* EONS names for NR and LTE */
            static const uint8_t eons_nr[] = {
                0x00, 0x03, 'J', 'i', 'o',
                0x00, 0x00, 0x00, 0x0a, 'J', 'i', 'o', ' ', 'T', 'r', 'u',
                'e', '5', 'G',
                0x00, 0x00, 0x00, 0x0a, 'J', 'i', 'o', ' ', 'T', 'r', 'u',
                'e', '5', 'G'
            };
            static const uint8_t eons_lte[] = {
                0x00, 0x03, 'J', 'i', 'o',
                0x00, 0x00, 0x00, 0x06, 'J', 'I', 'O', ' ', '4', 'G',
                0x00, 0x00, 0x00, 0x06, 'J', 'I', 'O', ' ', '4', 'G'
            };

            if (mock_flags & MOCK_EONS_LTE)
                n = qmi_put_tlv(rsp, 0, 0x10, eons_lte, sizeof(eons_lte));
            else
                n = qmi_put_tlv(rsp, 0, 0x10, eons_nr, sizeof(eons_nr));
            *rsp_len = n;
        } else if (msgid == 0x004D) { /* system info: LTE cell 405/861 */
            uint8_t si[64];
            size_t p = 0;

            si[p++] = 1; si[p++] = 1;   /* domain valid+value */
            si[p++] = 1; si[p++] = 1;   /* svc cap */
            si[p++] = 1; si[p++] = 0;   /* roaming */
            si[p++] = 1; si[p++] = 0;   /* forbidden */
            si[p++] = 1;                /* lac valid */
            si[p++] = 0x34; si[p++] = 0x12;
            si[p++] = 1;                /* cid valid */
            si[p++] = 0x01; si[p++] = 0x02; si[p++] = 0x03; si[p++] = 0x04;
            si[p++] = 0; si[p++] = 0; si[p++] = 0; /* reject */
            si[p++] = 1;                /* network id valid */
            memcpy(si + p, "405", 4); p += 4;
            memcpy(si + p, "861", 4); p += 4;
            si[p++] = 1;                /* tac valid */
            si[p++] = 0x21; si[p++] = 0x43;
            n = qmi_put_tlv(rsp, 0, 0x19, si, (uint16_t)p);
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_DMS:
        if (msgid == 0x0025) { /* Get IDs */
            n = qmi_put_tlv(rsp, 0, 0x11, "356938035643809", 16);
            n = qmi_put_tlv(rsp, n, 0x13, "01", 3);
            n = qmi_put_tlv(rsp, n, 0x12, "", 1);
            n = qmi_put_tlv(rsp, n, 0x10, "", 1);
            *rsp_len = n;
        } else if (msgid == 0x0023) { /* baseband revision */
            n = qmi_put_tlv(rsp, 0, 0x01, "MPSS.HI.2.5-01066-SDX55", 24);
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_WMS:
        if (msgid == 0x0020) { /* Raw Send -> message ref 7 */
            if (mock_flags & MOCK_SMS_ERR70)
                return 70;
            if (mock_flags & MOCK_SMS_RP38) {
                uint8_t ci[3] = { 38, 0x00, 0xff }; /* RP cause u16 LE, TP cause */

                n = qmi_put_tlv(rsp, 0, 0x12, ci, sizeof(ci));
                *rsp_len = n;
                return 50;
            }
            n = qmi_put_u16(rsp, 0, 0x01, 7);
            *rsp_len = n;
        } else if (msgid == 0x0022) { /* Raw Read: tag 0, fmt 6, pdu AA BB */
            uint8_t rd[6] = { 0, 6, 2, 0, 0xaa, 0xbb };

            n = qmi_put_tlv(rsp, 0, 0x01, rd, sizeof(rd));
            *rsp_len = n;
        } else if (msgid == 0x0048) {
            n = qmi_put_u8(rsp, 0, 0x10, 1); /* IMS transport up */
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_VOICE:
        if (msgid == 0x0020) { /* Dial -> call id 3 */
            if (mock_flags & MOCK_DIAL_FAIL)
                return 20;
            n = qmi_put_u8(rsp, 0, 0x10, 3);
            *rsp_len = n;
        } else if (msgid == 0x003A) { /* USSD reply */
            if (mock_flags & MOCK_USSD_GSM7) {
                /* "hello" packed as GSM 7-bit, dcs 2 */
                uint8_t u[7] = { 2, 5, 0xe8, 0x32, 0x9b, 0xfd, 0x06 };

                n = qmi_put_tlv(rsp, 0, 0x12, u, sizeof(u));
            } else {
                uint8_t u[7] = { 0, 5, 'h', 'e', 'l', 'l', 'o' };

                n = qmi_put_tlv(rsp, 0, 0x12, u, sizeof(u));
            }
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_WDS:
        if (msgid == 0x0020) { /* Start Network -> handle 0x1234 */
            if (mock_flags & MOCK_WDS_FAIL)
                return 5;
            n = qmi_put_u32(rsp, 0, 0x01, 0x1234);
            *rsp_len = n;
        } else if (msgid == 0x002D) { /* settings: 192.168.1.5/27, dns 8.8.8.8, mtu 1372 */
            uint8_t a[4] = { 5, 1, 168, 192 };
            uint8_t mask[4] = { 0xff, 0xff, 0xff, 0xe0 };
            uint8_t dns4[4] = { 8, 8, 8, 8 };

            n = qmi_put_tlv(rsp, 0, 0x1E, a, sizeof(a));
            n = qmi_put_tlv(rsp, n, 0x21, mask, sizeof(mask));
            n = qmi_put_tlv(rsp, n, 0x15, dns4, sizeof(dns4));
            n = qmi_put_u32(rsp, n, 0x29, 1372);
            *rsp_len = n;
        }
        return 0;
    case QMI_SVC_IMSA:
        if (msgid == 0x0020) {
            n = qmi_put_u8(rsp, 0, 0x12,
                           (mock_flags & MOCK_IMS_UNREG) ? 0 : 2);
            *rsp_len = n;
        } else if (msgid == 0x0021) {
            n = qmi_put_u8(rsp, 0, 0x11, 2); /* voice available */
            *rsp_len = n;
        }
        return 0;
    default:
        (void)put_result_ok(rsp);
        return 0;
    }
}

/* ---------------- RIL driving ---------------- */

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc,
                                   char **argv);

static const RIL_RadioFunctions *f;

static void verify_call_list(void)
{
    RIL_Call **calls = (RIL_Call **)snap.data;

    CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(RIL_Call *) &&
          calls[0] && calls[0]->index == 3 &&
          calls[0]->state == RIL_CALL_DIALING, "call list after dial");
}

static void verify_call_alerting(void)
{
    RIL_Call **calls = (RIL_Call **)snap.data;

    CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(RIL_Call *) &&
          calls[0] && calls[0]->state == RIL_CALL_ALERTING,
          "indication updates call state");
}

static void verify_incoming_call(void)
{
    RIL_Call **calls = (RIL_Call **)snap.data;

    CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(RIL_Call *) &&
          calls[0] && calls[0]->index == 4 &&
          calls[0]->state == RIL_CALL_INCOMING &&
          calls[0]->isMT == 1,
          "incoming MT call (direction=2) sets isMT=1 and state=INCOMING");
}

static void call(int req, void *data, size_t len, RIL_Token t)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
    int rc;

    snap_clear();
    pthread_mutex_lock(&done_mu);
    done_flag = 0;
    pthread_mutex_unlock(&done_mu);
    f->onRequest(req, data, len, t);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 8;
    pthread_mutex_lock(&done_mu);
    while (!done_flag) {
        rc = pthread_cond_timedwait(&done_cv, &done_mu, &ts);
        if (rc == ETIMEDOUT) {
            printf("FAIL completion timeout for request %d\n", req);
            failures++;
            break;
        }
    }
    pthread_mutex_unlock(&done_mu);
}

int smoke_main(void);

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--smoke"))
        return smoke_main();

    static const struct RIL_Env env = {
        .OnRequestComplete = on_complete,
        .OnUnsolicitedResponse = on_unsol,
        .RequestTimedCallback = timed_cb,
    };
    RIL_Token tok = (RIL_Token)(long)1;
    char *sms[2] = { (char *)"00", (char *)"0791..." };
    RIL_Dial dial = { .address = (char *)"+919999999999", .clir = 0 };
    char *dc[7] = { (char *)"0", (char *)"0", (char *)"jionet",
                    (char *)"", (char *)"", (char *)"0", (char *)"IP" };
    RIL_SMS_Response *smsr;
    RIL_Data_Call_Response_v11 *dcr;
    RIL_CardStatus_v1_5 *cs;
    RIL_SignalStrength_v12 *sig;

    f = RIL_Init(&env, 0, NULL);
    CHECK(f != NULL, "RIL_Init");

    {
        int on = 1;

        call(RIL_REQUEST_RADIO_POWER, &on, sizeof(on), tok);
        CHECK(snap.err == RIL_E_SUCCESS, "radio on completes");
    }

    uim_refresh();
    usleep(400000);

    /* IMSI and ICCID come from the mocked EF reads. */
    call(RIL_REQUEST_GET_IMSI, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.len == 16 &&
          !strcmp((char *)snap.data, "405861000000001"), "GET_IMSI decoded");

    {
        char num[24];

        uim_get_msisdn(num, sizeof(num));
        CHECK(!strcmp(num, "917890000000"), "EF_MSISDN decoded");
    }

    call(RIL_REQUEST_GET_SIM_STATUS, NULL, 0, tok);
    cs = (RIL_CardStatus_v1_5 *)snap.data;
    CHECK(snap.err == RIL_E_SUCCESS &&
          cs->base.base.base.card_state == RIL_CARDSTATE_PRESENT &&
          cs->base.base.iccid && !strcmp(cs->base.base.iccid,
                                         "89918610000000000000"),
          "GET_SIM_STATUS ICCID");

    /* USIM AKA: base64 in, base64 out, 0x88 APDU on UIM Send APDU. */
    {
        RIL_SimAuthentication auth = {
            .authContext = 0x81,
            .authData = (char *)"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=",
            .aid = (char *)"",
        };
        RIL_SIM_IO_Response *ar;

        mock_auth_apdu_ok = 0;
        call(RIL_REQUEST_SIM_AUTHENTICATION, &auth, sizeof(auth), tok);
        ar = (RIL_SIM_IO_Response *)snap.data;
        CHECK(snap.err == RIL_E_SUCCESS && ar->sw1 == 0x90 &&
              ar->sw2 == 0x00 && ar->simResponse &&
              !strcmp(ar->simResponse, "qrvM3Q=="),
              "SIM auth returns base64 AKA payload");
        CHECK(mock_auth_apdu_ok, "SIM auth uses USIM AKA APDU");
    }

    nas_refresh();
    usleep(600000);
    {
        int r = 0, rf = 0, mc = 0, mn = 0, ml = 0;
        bool hv = nas_get_serving(&r, NULL, &rf, &mc, &mn, &ml);

        printf("dbg serving have=%d reg=%d rif=%d mcc=%d mnc=%d\n", hv, r, rf,
               mc, mn);
    }
    expect_strings = REG_STATE_LEN;
    call(RIL_REQUEST_VOICE_REGISTRATION_STATE, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.nstr == REG_STATE_LEN &&
          !strcmp(snap.strs[0], "1") && !strcmp(snap.strs[3], "14") &&
          !strcmp(snap.strs[17], "405861"), "registration cache");

    call(RIL_REQUEST_SIGNAL_STRENGTH, NULL, 0, tok);
    sig = (RIL_SignalStrength_v12 *)snap.data;
    CHECK(snap.len == sizeof(*sig) && sig->LTE_SignalStrength.rsrp == 89 &&
          sig->LTE_SignalStrength.rsrq == 11, "signal strength");

    /* LTE with empty NR sentinel TLVs must not report an empty NR signal. */
    mock_flags |= MOCK_SIG_LTE_NR;
    nas_refresh();
    usleep(600000);
    call(RIL_REQUEST_SIGNAL_STRENGTH, NULL, 0, tok);
    sig = (RIL_SignalStrength_v12 *)snap.data;
    CHECK(snap.len == sizeof(*sig) && sig->LTE_SignalStrength.rsrp == 89 &&
          sig->LTE_SignalStrength.rsrq == 11 &&
          sig->NR_SignalStrength.ssRsrp == INT_MAX,
          "signal strength ignores NR sentinel on LTE");
    mock_flags &= ~MOCK_SIG_LTE_NR;

    expect_strings = 3;
    call(RIL_REQUEST_OPERATOR, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.nstr == 3 &&
          !strcmp(snap.strs[0], "Jio True5G") &&
          !strcmp(snap.strs[1], "Jio True5G") &&
          !strcmp(snap.strs[2], "405861"),
          "operator name from NAS EONS");

    /* Same path on LTE: EONS long/short are "JIO 4G". */
    mock_flags |= MOCK_EONS_LTE;
    nas_refresh();
    usleep(600000);
    expect_strings = 3;
    call(RIL_REQUEST_OPERATOR, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.nstr == 3 &&
          !strcmp(snap.strs[0], "JIO 4G") &&
          !strcmp(snap.strs[1], "JIO 4G") &&
          !strcmp(snap.strs[2], "405861"),
          "LTE EONS operator name");
    mock_flags &= ~MOCK_EONS_LTE;

    {
        RIL_SIM_IO_v6 io;
        RIL_SIM_IO_Response *ior;

        memset(&io, 0, sizeof(io));
        io.command = 0xB0;
        io.fileid = 0x6F46;
        io.path = (char *)"3F007F20";
        call(RIL_REQUEST_SIM_IO, &io, sizeof(io), tok);
        ior = (RIL_SIM_IO_Response *)snap.data;
        CHECK(snap.err == RIL_E_SUCCESS && ior->sw1 == 0x90 && ior->sw2 == 0 &&
              ior->simResponse && strlen(ior->simResponse) > 0,
              "SIM_IO EF_SPN read");
    }

    call(RIL_REQUEST_SEND_SMS, sms, sizeof(sms), tok);
    smsr = (RIL_SMS_Response *)snap.data;
    CHECK(snap.err == RIL_E_SUCCESS && smsr->messageRef == 7, "SMS send ref");
    {
        uint16_t l = 0;
        const uint8_t *v;
        RIL_IMS_SMS_Message ims;
        char *gsm[2] = { sms[0], sms[1] };

        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x13, &l);
        CHECK(v && l == 1 && v[0] == 1, "CS SMS sets sms_on_ims");

        memset(&ims, 0, sizeof(ims));
        ims.tech = RADIO_TECH_3GPP;
        ims.message.gsmMessage = gsm;
        call(RIL_REQUEST_IMS_SEND_SMS, &ims, sizeof(ims), tok);
        smsr = (RIL_SMS_Response *)snap.data;
        CHECK(snap.err == RIL_E_SUCCESS && smsr->messageRef == 7,
              "IMS SMS send ref");
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x13, &l);
        CHECK(v && l == 1 && v[0] == 1, "IMS SMS sets sms_on_ims");
    }

    call(RIL_REQUEST_DIAL, &dial, sizeof(dial), tok);
    CHECK(snap.err == RIL_E_SUCCESS, "dial accepted");
    on_resp_verify = verify_call_list;
    call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);

    call(RIL_REQUEST_SETUP_DATA_CALL, dc, sizeof(dc), tok);
    dcr = (RIL_Data_Call_Response_v11 *)snap.data;
    CHECK(snap.err == RIL_E_SUCCESS && dcr->status == 0 &&
          !strcmp(dcr->addresses, "192.168.1.5/27") &&
          !strcmp(dcr->dnses, "8.8.8.8") && dcr->mtu == 1372,
          "data setup addresses");
    {
        int first_cid = dcr->cid;
        char *dc2[7] = { (char *)"0", (char *)"0", (char *)"ims",
                         (char *)"", (char *)"", (char *)"0",
                         (char *)"IPV4V6" };
        RIL_Data_Call_Response_v11 *d2;

        call(RIL_REQUEST_SETUP_DATA_CALL, dc2, sizeof(dc2), tok);
        d2 = (RIL_Data_Call_Response_v11 *)snap.data;
        CHECK(snap.err == RIL_E_SUCCESS && d2->status == 0 &&
              d2->cid != first_cid && d2->active == 1,
              "second PDN gets its own cid");

        call(RIL_REQUEST_DATA_CALL_LIST, NULL, 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS &&
              snap.len == 2 * sizeof(RIL_Data_Call_Response_v11),
              "data call list reports both PDNs");

        {
            char cidbuf[16];
            char *deact[2];

            snprintf(cidbuf, sizeof(cidbuf), "%d", first_cid);
            deact[0] = cidbuf;
            deact[1] = (char *)"0";
            call(RIL_REQUEST_DEACTIVATE_DATA_CALL, deact, sizeof(deact), tok);
            CHECK(snap.err == RIL_E_SUCCESS, "deactivate by cid");
        }
        call(RIL_REQUEST_DATA_CALL_LIST, NULL, 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS &&
              snap.len == sizeof(RIL_Data_Call_Response_v11),
              "deactivate removes only that PDN");
    }

    call(RIL_REQUEST_GET_CELL_INFO_LIST, NULL, 0, tok);
    {
        RIL_CellInfo_v12 *ci = (RIL_CellInfo_v12 *)snap.data;

        CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(*ci) &&
              ci->cellInfoType == RIL_CELL_INFO_TYPE_LTE &&
              ci->CellInfo.lte.cellIdentityLte.mcc == 405 &&
              ci->CellInfo.lte.cellIdentityLte.tac == 0x4321,
              "cell info from NAS 0x4D");
    }

    /* Stock dial frame: call type 0x16 == 1, IMS domain 0x18 == 3, no 0x11. */
    ims_poll_once();
    CHECK(ims_reg_status() == 2, "IMSA reports registered");
    call(RIL_REQUEST_DIAL, &dial, sizeof(dial), tok);
    {
        uint16_t l11 = 0, l16 = 0, l18 = 0;
        const uint8_t *v11 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x11, &l11);
        const uint8_t *v16 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x16, &l16);
        const uint8_t *v18 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x18, &l18);
        uint32_t ctype = (v16 && l16 == 4) ? (v16[0] | (v16[1] << 8) | (v16[2] << 16) | (v16[3] << 24)) : 0;

        CHECK(last_req.svc == QMI_SVC_VOICE && last_req.msg == 0x0020 &&
              !v11 && ctype == 1 && v18 && l18 == 8 && v18[0] == 3,
              "registered dial uses call_type 1 and IMS domain 3");
    }

    /* MT over transfer route: unsolicited PDU + SEND_ACK encoding. */
    {
        uint8_t tlvs[64];
        uint8_t tr[12] = { 0, 0x44, 0x33, 0x22, 0x11, 6, 4, 0,
                           1, 2, 3, 4 };
        size_t n = 0;
        uint16_t l = 0;
        const uint8_t *v;

        n = qmi_put_tlv(tlvs, n, 0x11, tr, sizeof(tr));
        n = qmi_put_tlv(tlvs, n, 0x15, "0791", 4);
        n = qmi_put_u8(tlvs, n, 0x16, 1);
        qmi_test_deliver_indication(&g_wms, 0x0001, tlvs, n);
        CHECK(last_unsol.id == RIL_UNSOL_RESPONSE_NEW_SMS &&
              !strcmp((char *)last_unsol.data, "079101020304"),
              "MT transfer-route delivered");

        {
            int ok = 1;

            call(RIL_REQUEST_SMS_ACKNOWLEDGE, &ok, sizeof(ok), tok);
        }
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x01, &l);
        CHECK(last_req.msg == 0x0037 && v && l == 6 && v[0] == 0x44 &&
              v[4] == 1 && v[5] == 1, "SEND_ACK for transfer-route");
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x12, &l);
        CHECK(v && l == 1 && v[0] == 1, "SEND_ACK carries sms_on_ims");
    }

    /* Stored MT: RAW_READ reply delivered, DELETE uses the IDL layout. */
    {
        uint8_t tlvs[32];
        uint8_t st[5] = { 1, 5, 0, 0, 0 };
        size_t n = 0;
        uint16_t l = 0;
        const uint8_t *v;

        n = qmi_put_tlv(tlvs, n, 0x10, st, sizeof(st));
        n = qmi_put_u8(tlvs, n, 0x12, 1);
        qmi_test_deliver_indication(&g_wms, 0x0001, tlvs, n);
        usleep(500000);
        CHECK(last_unsol.id == RIL_UNSOL_RESPONSE_NEW_SMS &&
              !strcmp((char *)last_unsol.data, "aabb"), "stored MT delivered");

        {
            int ok = 1;

            call(RIL_REQUEST_SMS_ACKNOWLEDGE, &ok, sizeof(ok), tok);
        }
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x01, &l);
        CHECK(last_req.msg == 0x0024 && v && l == 1 && v[0] == 1,
              "DELETE storage TLV");
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x10, &l);
        CHECK(v && l == 4 && v[0] == 5, "DELETE index TLV");
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x11, &l);
        CHECK(v && l == 1 && v[0] == 0, "DELETE tag TLV");
        v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x12, &l);
        CHECK(v && l == 1 && v[0] == 1, "DELETE mode TLV");
    }

    /* Call-state indications drive and retire the call list. */
    {
        uint8_t tlvs[16];
        uint8_t ci[8] = { 1, 3, 5, 0, 1, 0, 0, 0 }; /* id3 ALERTING, MO */

        qmi_put_tlv(tlvs, 0, 0x01, ci, sizeof(ci));
        qmi_test_deliver_indication(&g_voice, 0x002E, tlvs,
                                    qmi_put_tlv(tlvs, 0, 0x01, ci, sizeof(ci)));
        on_resp_verify = verify_call_alerting;
        call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);
        ci[2] = 9; /* END */
        qmi_test_deliver_indication(&g_voice, 0x002E, tlvs,
                                    qmi_put_tlv(tlvs, 0, 0x01, ci, sizeof(ci)));
        call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);
        CHECK(snap.len == 0, "END retires the call");

        /* Incoming call indication: verifies direction=2 sets isMT=1 */
        uint8_t mt_ci[8] = { 1, 4, 2, 0, 2, 0, 0, 0 }; /* id 4, state 2 (INCOMING), direction 2 (MT) */
        qmi_test_deliver_indication(&g_voice, 0x002E, tlvs,
                                    qmi_put_tlv(tlvs, 0, 0x01, mt_ci, sizeof(mt_ci)));
        on_resp_verify = verify_incoming_call;
        call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);
        mt_ci[2] = 9; /* END */
        qmi_test_deliver_indication(&g_voice, 0x002E, tlvs,
                                    qmi_put_tlv(tlvs, 0, 0x01, mt_ci, sizeof(mt_ci)));
        call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);
        CHECK(snap.len == 0, "END retires incoming call");
    }

    /* EN-DC / 5G NSA available in DATA_REGISTRATION_STATE */
    expect_strings = 15;
    call(RIL_REQUEST_DATA_REGISTRATION_STATE, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.nstr == 15 &&
          !strcmp(snap.strs[14], "1"),
          "DATA_REGISTRATION_STATE reports EN-DC / 5G NSA available (string 14 == '1')");

    /* NR Signal Strength: units differ from LTE (dBm / 0.1 dB). */
    mock_flags |= MOCK_SIG_NR;
    nas_refresh();
    usleep(600000);
    call(RIL_REQUEST_SIGNAL_STRENGTH, NULL, 0, tok);
    {
        RIL_SignalStrength_v12 *ss = (RIL_SignalStrength_v12 *)snap.data;
        CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(*ss) &&
              ss->NR_SignalStrength.ssRsrp == 91 &&
              ss->NR_SignalStrength.ssRsrq == 11 &&
              ss->NR_SignalStrength.ssSinr == 23 &&
              ss->LTE_SignalStrength.rsrp == INT_MAX,
              "SIGNAL_STRENGTH maps NR units (rsrp 91, rsrq 11, sinr 23)");
    }
    mock_flags &= ~MOCK_SIG_NR;

    /* USSD reply becomes RIL_UNSOL_ON_USSD. */
    call(RIL_REQUEST_SEND_USSD, (void *)"*123#", 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && last_unsol.id == RIL_UNSOL_ON_USSD &&
          last_unsol.strs[0] && !strcmp(last_unsol.strs[0], "0") &&
          last_unsol.strs[1] && !strcmp(last_unsol.strs[1], "hello"),
          "USSD reply delivered");

    /* Network scan result. */
    call(RIL_REQUEST_START_NETWORK_SCAN, NULL, 0, tok);
    usleep(600000);
    CHECK(last_unsol.id == RIL_UNSOL_NETWORK_SCAN_RESULT, "scan result unsol");
    {
        RIL_NetworkScanResult *r = (RIL_NetworkScanResult *)last_unsol.data;

        CHECK(last_unsol.len >= sizeof(*r) && r->network_infos_length == 2,
              "scan lists two operators");
    }


    /* DTMF TLV layout and USSD indications (upstream IDL). */
    {
        uint8_t tlvs[16];
        char *d = (char *)"5";

        call(RIL_REQUEST_DIAL, &dial, sizeof(dial), tok);
        {
            uint8_t ci2[8] = { 1, 3, 3, 0, 1, 0, 0, 0 }; /* id3 CONVERSATION */

            qmi_put_tlv(tlvs, 0, 0x01, ci2, sizeof(ci2));
            qmi_test_deliver_indication(&g_voice, 0x002E, tlvs,
                                        qmi_put_tlv(tlvs, 0, 0x01, ci2, sizeof(ci2)));
        }
        call(RIL_REQUEST_DTMF, d, 2, tok);
        {
            uint16_t l = 0;
            const uint8_t *v = qmi_find_tlv(last_req.tlvs, last_req.len, 0x01, &l);

            CHECK(last_req.msg == 0x0028 && v && l == 3 && v[0] == 3 &&
                  v[1] == 1 && v[2] == '5', "DTMF length-prefixed string");
        }
        call(RIL_REQUEST_HANGUP, &(int){3}, sizeof(int), tok);
    }
    {
        uint8_t tlvs[32];
        uint8_t act = 2; /* QMI_VOICE_USER_ACTION_REQUIRED */
        uint8_t uss[7] = { 0, 5, 'h', 'e', 'l', 'l', 'o' };
        size_t n = 0;

        n = qmi_put_tlv(tlvs, n, 0x01, &act, 1);
        n = qmi_put_tlv(tlvs, n, 0x10, uss, sizeof(uss));
        qmi_test_deliver_indication(&g_voice, 0x003E, tlvs, n);
        CHECK(last_unsol.id == RIL_UNSOL_ON_USSD && last_unsol.strs[0] &&
              !strcmp(last_unsol.strs[0], "1") && last_unsol.strs[1] &&
              !strcmp(last_unsol.strs[1], "hello"),
              "USSD menu indication delivered");
        qmi_test_deliver_indication(&g_voice, 0x003D, NULL, 0);
        CHECK(last_unsol.id == RIL_UNSOL_ON_USSD && last_unsol.strs[0] &&
              !strcmp(last_unsol.strs[0], "0"), "USSD release is stock NOTIFY");

        /* Stock cancel: QMI 0x3C only, no USSD unsolicited. */
        {
            int before = last_unsol.count;
            int dummy = 0;

            call(RIL_REQUEST_CANCEL_USSD, &dummy, sizeof(dummy), tok);
            CHECK(snap.err == RIL_E_SUCCESS &&
                  last_req.svc == QMI_SVC_VOICE && last_req.msg == 0x003C &&
                  last_unsol.count == before,
                  "cancel sends 0x3C without an unsol");
        }
    }

    /* USSD text decoding: GSM 7-bit, UCS-2, and the originate reply path. */
    {
        uint8_t tlvs[32];
        uint8_t act = 2;
        uint8_t g7[7] = { 2, 5, 0xe8, 0x32, 0x9b, 0xfd, 0x06 };
        uint8_t u16[5] = { 4, 'O', 0, 'K', 0 };
        size_t n = 0;

        n = qmi_put_tlv(tlvs, n, 0x01, &act, 1);
        n = qmi_put_tlv(tlvs, n, 0x10, g7, sizeof(g7));
        qmi_test_deliver_indication(&g_voice, 0x003E, tlvs, n);
        CHECK(last_unsol.strs[0] && !strcmp(last_unsol.strs[0], "1") &&
              last_unsol.strs[1] && !strcmp(last_unsol.strs[1], "hello"),
              "USSD GSM 7-bit decoded");

        n = qmi_put_tlv(tlvs, 0, 0x01, &act, 1);
        n = qmi_put_tlv(tlvs, n, 0x11, u16, sizeof(u16));
        qmi_test_deliver_indication(&g_voice, 0x003E, tlvs, n);
        CHECK(last_unsol.strs[1] && !strcmp(last_unsol.strs[1], "OK"),
              "USSD UCS-2 decoded");

        mock_flags |= MOCK_USSD_GSM7;
        call(RIL_REQUEST_SEND_USSD, (void *)"*123#", 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS && last_unsol.strs[1] &&
              !strcmp(last_unsol.strs[1], "hello"),
              "USSD originate reply decoded");
        mock_flags &= ~MOCK_USSD_GSM7;
    }

    /* Device identity and baseband from DMS. */
    call(RIL_REQUEST_GET_IMEI, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS &&
          !strcmp((char *)snap.data, "356938035643809"), "IMEI");
    call(RIL_REQUEST_GET_IMEISV, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && !strcmp((char *)snap.data, "3569380356438001"),
          "IMEISV");
    expect_strings = 4;
    call(RIL_REQUEST_DEVICE_IDENTITY, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.nstr == 4 &&
          !strcmp(snap.strs[0], "356938035643809") &&
          !strcmp(snap.strs[1], "3569380356438001"), "DEVICE_IDENTITY");
    call(RIL_REQUEST_BASEBAND_VERSION, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS &&
          strstr((char *)snap.data, "MPSS"), "baseband revision");

    /* Error paths: retries, fallbacks and honest failures. */
    mock_flags |= MOCK_IMS_UNREG;
    ims_poll_once();
    CHECK(ims_reg_status() == 0, "IMSA timeout -> unregistered");
    call(RIL_REQUEST_DIAL, &dial, sizeof(dial), tok);
    {
        uint16_t l16 = 0, l18 = 0;
        const uint8_t *v16 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x16, &l16);
        const uint8_t *v18 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x18, &l18);
        uint32_t ctype = (v16 && l16 == 4) ? (v16[0] | (v16[1] << 8) | (v16[2] << 16) | (v16[3] << 24)) : 0;

        CHECK(ctype == 1 && v18 && l18 == 8 && v18[0] == 1,
              "dial falls back to CS domain 1");
    }
    mock_flags &= ~MOCK_IMS_UNREG;

    /* Emergency dial: stock-compatible frame + address pass-through. */
    {
        RIL_EmergencyDial edial = {
            .dialInfo = { .address = (char *)"112", .clir = 0 },
            .categories = 1,
            .routing = 1,
        };
        call(RIL_REQUEST_EMERGENCY_DIAL, &edial, sizeof(edial), tok);
        uint16_t ln = 0, l18 = 0;
        const uint8_t *vn = qmi_find_tlv(last_req.tlvs, last_req.len, 0x01, &ln);
        const uint8_t *v18 = qmi_find_tlv(last_req.tlvs, last_req.len, 0x18, &l18);
        CHECK(snap.err == RIL_E_SUCCESS && last_req.svc == QMI_SVC_VOICE &&
              last_req.msg == 0x0020 && vn && ln == 3 &&
              !memcmp(vn, "112", 3) && v18 && l18 == 8,
              "emergency dial uses stock TLVs and passes the number");
    }

    /* Last call fail cause check */
    call(RIL_REQUEST_LAST_CALL_FAIL_CAUSE, NULL, 0, tok);
    CHECK(snap.err == RIL_E_SUCCESS && snap.len == sizeof(int),
          "LAST_CALL_FAIL_CAUSE returns int cause");

    /* Stubs for Android Telephony telephony checks */
    {
        int allow = 1;
        call(RIL_REQUEST_ALLOW_DATA, &allow, sizeof(allow), tok);
        CHECK(snap.err == RIL_E_SUCCESS, "ALLOW_DATA stub success");

        int screen = 1;
        call(RIL_REQUEST_SCREEN_STATE, &screen, sizeof(screen), tok);
        CHECK(snap.err == RIL_E_SUCCESS, "SCREEN_STATE stub success");

        int loc = 1;
        call(RIL_REQUEST_SET_LOCATION_UPDATES, &loc, sizeof(loc), tok);
        CHECK(snap.err == RIL_E_SUCCESS, "SET_LOCATION_UPDATES stub success");
    }

    mock_flags |= MOCK_SMS_ERR70;
    call(RIL_REQUEST_SEND_SMS, sms, sizeof(sms), tok);
    CHECK(snap.err == RIL_E_SMS_SEND_FAIL_RETRY, "QMI 70 is retryable");
    mock_flags &= ~MOCK_SMS_ERR70;

    mock_flags |= MOCK_SMS_RP38;
    call(RIL_REQUEST_SEND_SMS, sms, sizeof(sms), tok);
    smsr = (RIL_SMS_Response *)snap.data;
    CHECK(snap.err == RIL_E_SMS_SEND_FAIL_RETRY && smsr->errorCode == 38,
          "RP cause 38 retries");
    mock_flags &= ~MOCK_SMS_RP38;

    mock_flags |= MOCK_DIAL_FAIL;
    call(RIL_REQUEST_DIAL, &dial, sizeof(dial), tok);
    CHECK(snap.err == RIL_E_GENERIC_FAILURE, "dial failure reported");
    mock_flags &= ~MOCK_DIAL_FAIL;

    mock_flags |= MOCK_WDS_FAIL;
    call(RIL_REQUEST_SETUP_DATA_CALL, dc, sizeof(dc), tok);
    dcr = (RIL_Data_Call_Response_v11 *)snap.data;
    CHECK(snap.err == RIL_E_SUCCESS && dcr->status == 0xffff,
          "data failure sets PDP_FAIL");
    mock_flags &= ~MOCK_WDS_FAIL;

    mock_flags |= MOCK_UIM_ABSENT;
    uim_refresh();
    usleep(400000);
    call(RIL_REQUEST_GET_SIM_STATUS, NULL, 0, tok);
    cs = (RIL_CardStatus_v1_5 *)snap.data;
    CHECK(snap.err == RIL_E_SUCCESS &&
          cs->base.base.base.card_state == RIL_CARDSTATE_ABSENT,
          "absent SIM reported");
    mock_flags &= ~MOCK_UIM_ABSENT;

    {
        int off = 0;

        call(RIL_REQUEST_RADIO_POWER, &off, sizeof(off), tok);
        CHECK(snap.err == RIL_E_SUCCESS, "radio off completes");
        call(RIL_REQUEST_DATA_CALL_LIST, NULL, 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS && snap.len == 0,
              "radio off clears PDN list");
        call(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS && snap.len == 0,
              "radio off clears calls");
        call(RIL_REQUEST_IMS_REGISTRATION_STATE, NULL, 0, tok);
        CHECK(snap.err == RIL_E_SUCCESS && snap.len == 2 * sizeof(int) &&
              ((int *)snap.data)[0] == 0,
              "radio off clears IMS registration");
    }

    printf("%s (%d failures)\n", failures ? "MOCK TEST FAILED" : "MOCK TEST OK",
           failures);
    return failures ? 1 : 0;
}
