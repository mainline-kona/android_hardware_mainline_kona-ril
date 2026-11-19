/*
 * voice.c — QMI voice calls.
 *
 * Wire layouts from the in-tree IDL plus the stock blob:
 *   0x20 Dial Call  : TLV 0x01 number; TLV 0x10 CLIR u8 (0 default, 1 invoke,
 *                     2 suppress); TLV 0x11 service type u8 (1 CS/LTE,
 *                     3 IMS, 4 CDMA) — the last two are not in the trimmed
 *                     in-tree IDL but are mandatory for this modem.
 *   0x21 End Call / 0x22 Answer Call : TLV 0x01 call id u8.
 *   0x28 Burst DTMF : TLV 0x01 {u8 call id, u8 length, digit bytes}.
 *   0x0003 Indication Register : TLVs 0x12, 0x13 and 0x16 = 1.
 *   0x002E All Call Status indication : TLV 0x01 call info array
 *           {u8 n, per call {u8 id, u8 state, u8 type, u8 dir, u8 mode,
 *            u8 mpty, u8 als}}, TLV 0x10 remote party numbers.
 */
#define LOG_TAG "kebab-rild"

#include "voice.h"
#include "ims.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <log/log.h>

#define VOICE_INDICATION_REGISTER 0x0003
#define VOICE_DIAL_CALL           0x0020
#define VOICE_END_CALL            0x0021
#define VOICE_ANSWER_CALL         0x0022
#define VOICE_BURST_DTMF          0x0028
#define VOICE_ALL_CALL_STATUS     0x002E

#define VOICECALL_PATH "/vendor/bin/kebab-voicecall"
#define MAX_CALLS 8

struct vcall {
    int active;
    int id;
    int state;      /* QMI call state */
    int direction;
    int is_mpty;
    int als;
    int number_pres;
    char number[64];
};

static void uss_to_utf8(uint8_t dcs, const uint8_t *d, size_t n, char *out,
                        size_t max);
static void uss_utf16_tlv(const uint8_t *v, uint16_t l, char *out, size_t max);

static struct vcall s_calls[MAX_CALLS];
static pthread_mutex_t s_calls_mu = PTHREAD_MUTEX_INITIALIZER;

static pid_t s_vc_pid = -1;
static int s_vc_ready_fd = -1;
static pthread_mutex_t s_vc_mu = PTHREAD_MUTEX_INITIALIZER;

void voice_path_set(int on)
{
    pthread_mutex_lock(&s_vc_mu);
    if (s_vc_pid > 0 && waitpid(s_vc_pid, NULL, WNOHANG) == s_vc_pid) {
        if (s_vc_ready_fd >= 0) {
            close(s_vc_ready_fd);
            s_vc_ready_fd = -1;
        }
        s_vc_pid = -1;
    }
    if (on && s_vc_pid < 0 && access(VOICECALL_PATH, X_OK) == 0) {
        int fds[2] = { -1, -1 };
        pid_t pid;

        if (pipe(fds) != 0)
            ALOGE("voice path pipe: %s", strerror(errno));
        pid = fork();
        if (pid == 0) {
            if (fds[1] >= 0) {
                dup2(fds[1], 3);
                close(fds[0]);
                close(fds[1]);
            }
            execl(VOICECALL_PATH, VOICECALL_PATH, (char *)NULL);
            _exit(127);
        }
        if (fds[1] >= 0)
            close(fds[1]);
        if (pid > 0) {
            s_vc_pid = pid;
            s_vc_ready_fd = fds[0];
            ALOGI("kebab-voicecall started");
        } else if (fds[0] >= 0) {
            close(fds[0]);
        }
    } else if (!on && s_vc_pid > 0) {
        if (s_vc_ready_fd >= 0) {
            close(s_vc_ready_fd);
            s_vc_ready_fd = -1;
        }
        kill(s_vc_pid, SIGTERM);
        waitpid(s_vc_pid, NULL, 0);
        s_vc_pid = -1;
        ALOGI("kebab-voicecall stopped");
    }
    pthread_mutex_unlock(&s_vc_mu);
}

int voice_path_wait(int timeout_ms)
{
    char c;
    int fd;

    pthread_mutex_lock(&s_vc_mu);
    fd = s_vc_ready_fd;
    pthread_mutex_unlock(&s_vc_mu);
    if (fd < 0)
        return -1;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    if (poll(&pfd, 1, timeout_ms) <= 0)
        return -1;
    if (read(fd, &c, 1) != 1)
        return -1;
    return 0;
}

static int qmi_state_to_ril(int qmi_state)
{
    switch (qmi_state) {
    case 1: return RIL_CALL_DIALING;
    case 2: return RIL_CALL_INCOMING;
    case 3: return RIL_CALL_ACTIVE;
    case 4: return RIL_CALL_DIALING;
    case 5: return RIL_CALL_ALERTING;
    case 6: return RIL_CALL_HOLDING;
    case 7: return RIL_CALL_WAITING;
    default: return -1;
    }
}

static void parse_calls(const uint8_t *ci, size_t clen, const uint8_t *rn,
                        size_t rnlen)
{
    uint8_t seen[MAX_CALLS];
    int seen_n = 0;

    pthread_mutex_lock(&s_calls_mu);
    if (ci && clen >= 1) {
        int count = ci[0];

        for (int i = 0; i < count; i++) {
            size_t off = 1 + (size_t)i * 7;
            int slot = -1;

            if (off + 7 > clen)
                break;
            uint8_t id = ci[off];
            uint8_t state = ci[off + 1];

            if (seen_n < MAX_CALLS)
                seen[seen_n++] = id;
            for (int s = 0; s < MAX_CALLS; s++)
                if (s_calls[s].active && s_calls[s].id == id) {
                    slot = s;
                    break;
                }
            if (slot < 0 && state != 9) {
                for (int s = 0; s < MAX_CALLS; s++)
                    if (!s_calls[s].active) {
                        slot = s;
                        memset(&s_calls[s], 0, sizeof(s_calls[s]));
                        s_calls[s].id = id;
                        break;
                    }
            }
            if (slot < 0)
                continue;
            if (state == 9) {
                for (int s = 0; s < MAX_CALLS; s++)
                    if (s_calls[s].active && s_calls[s].id == id)
                        s_calls[s].active = 0;
            } else {
                for (int s = 0; s < MAX_CALLS; s++)
                    if (s != slot && s_calls[s].active && s_calls[s].id == id)
                        s_calls[s].active = 0;
                s_calls[slot].active = 1;
                s_calls[slot].id = id;
                s_calls[slot].state = state;
                s_calls[slot].direction = ci[off + 3];
                s_calls[slot].is_mpty = ci[off + 5];
                s_calls[slot].als = ci[off + 6];
            }
        }
    }
    /* Prune only when the indication actually carried a call list; an
     * indication without TLV 0x01 must not tear down every active call. */
    if (ci && clen >= 1) {
        for (int s = 0; s < MAX_CALLS; s++) {
            int found = 0;

            if (!s_calls[s].active)
                continue;
            for (int j = 0; j < seen_n; j++)
                if (seen[j] == s_calls[s].id)
                    found = 1;
            if (!found)
                s_calls[s].active = 0;
        }
    }
    if (rn && rnlen >= 1) {
        int count = rn[0];
        size_t off = 1;

        for (int i = 0; i < count && off + 3 <= rnlen; i++) {
            uint8_t id = rn[off];
            uint8_t pres = rn[off + 1];
            uint8_t nl = rn[off + 2];

            off += 3;
            if (off + nl > rnlen)
                break;
            for (int s = 0; s < MAX_CALLS; s++)
                if (s_calls[s].active && s_calls[s].id == id) {
                    s_calls[s].number_pres = pres;
                    if (nl > 0 && nl < sizeof(s_calls[s].number)) {
                        memcpy(s_calls[s].number, rn + off, nl);
                        s_calls[s].number[nl] = '\0';
                    }
                    break;
                }
            off += nl;
        }
    }
    pthread_mutex_unlock(&s_calls_mu);
}

static void on_calls_changed(void)
{
    int active = 0;

    pthread_mutex_lock(&s_calls_mu);
    for (int s = 0; s < MAX_CALLS; s++)
        if (s_calls[s].active && qmi_state_to_ril(s_calls[s].state) >= 0)
            active++;
    pthread_mutex_unlock(&s_calls_mu);
    voice_path_set(active > 0);
    kebab_unsol(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
}

static void voice_ind_cb(struct qmi_client *c, uint16_t msg,
                         const uint8_t *tlvs, size_t len)
{
    (void)c;
    if (msg == VOICE_ALL_CALL_STATUS) {
        uint16_t cl = 0, rl = 0;
        const uint8_t *ci = qmi_find_tlv(tlvs, len, 0x01, &cl);
        const uint8_t *rn = qmi_find_tlv(tlvs, len, 0x10, &rl);

        parse_calls(ci, cl, rn, rl);
        on_calls_changed();
    } else if (msg == 0x003E) { /* USSD (network-initiated / session menu) */
        uint16_t al = 0, dl = 0, ul = 0;
        const uint8_t *action = qmi_find_tlv(tlvs, len, 0x01, &al);
        const uint8_t *data = qmi_find_tlv(tlvs, len, 0x10, &dl);
        const uint8_t *utf16 = qmi_find_tlv(tlvs, len, 0x11, &ul);
        char text[400];
        char *payload[2];

        if (utf16) {
            uss_utf16_tlv(utf16, ul, text, sizeof(text));
        } else if (data && dl >= 2) {
            size_t sl = data[1];

            if (sl > dl - 2)
                sl = dl - 2;
            uss_to_utf8(data[0], data + 2, sl, text, sizeof(text));
        } else {
            text[0] = '\0';
        }
        /* QmiVoiceUserAction: 0 unknown, 1 not required, 2 required.
         * RIL USSD codes: "0" notify/no action, "1" further action. */
        payload[0] = (action && al && action[0] == 2) ? (char *)"1"
                                                      : (char *)"0";
        payload[1] = text;
        kebab_unsol(RIL_UNSOL_ON_USSD, payload, sizeof(payload));
    } else if (msg == 0x003D) { /* Release USSD: stock sends NOTIFY, empty */
        char *payload[2] = { (char *)"0", (char *)"" };

        kebab_unsol(RIL_UNSOL_ON_USSD, payload, sizeof(payload));
    }
}

/* Runs on its own thread: qmi_transact() must never run on the client thread
 * that delivers up_cb (the response could not be read there). */
static void *voice_register_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 15 && qmi_client_is_up(&g_voice); i++) {
        uint8_t req[16];
        size_t n = qmi_put_u8(req, 0, 0x12, 1); /* SS notifications */
        int rc;

        n = qmi_put_u8(req, n, 0x13, 1); /* call notifications */
        n = qmi_put_u8(req, n, 0x16, 1); /* USSD notifications */
        rc = qmi_transact(&g_voice, VOICE_INDICATION_REGISTER, req, n, NULL,
                          NULL, 5000);
        if (rc == 0)
            return NULL;
        ALOGE("VOICE: indication register failed %d (attempt %d)", rc, i + 1);
        sleep(1);
    }
    return NULL;
}

static void voice_up_cb(struct qmi_client *c, bool up)
{
    pthread_t t;

    (void)c;
    if (up) {
        if (pthread_create(&t, NULL, voice_register_thread, NULL) == 0)
            pthread_detach(t);
        return;
    }
    /* Modem restarted: no call survives; drop state and the audio helper. */
    voice_clear_calls();
}

void voice_clear_calls(void)
{
    pthread_mutex_lock(&s_calls_mu);
    memset(s_calls, 0, sizeof(s_calls));
    pthread_mutex_unlock(&s_calls_mu);
    voice_path_set(0);
    kebab_unsol(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
}

void voice_init(void)
{
    if (qmi_client_start(&g_voice, QMI_SVC_VOICE, voice_ind_cb, voice_up_cb) != 0)
        ALOGE("VOICE: client start failed");
}

static int s_last_call_fail_cause = 16; /* CALL_FAIL_NORMAL_CLEARING */

int voice_get_last_fail_cause(void)
{
    pthread_mutex_lock(&s_calls_mu);
    int c = s_last_call_fail_cause;
    pthread_mutex_unlock(&s_calls_mu);
    return c;
}

static int voice_dial_internal(const char *number, int clir, bool is_emergency, int *call_id)
{
    uint8_t req[192], rsp[64];
    size_t n = 0, rl = sizeof(rsp);
    uint8_t clir_v = (uint8_t)(clir >= 1 && clir <= 2 ? clir : 0);
    bool is_ims = ims_voip_registered();
    /*
     * Stock Qualcomm QMI VOICE DIAL_CALL (0x0020), captured on SDX55:
     *   TLV 0x01: Calling Number (string)
     *   TLV 0x10: CLIR type u8 (0 default, 1 suppression, 2 invocation)
     *   TLV 0x16: Call type u32, 1 = voice
     *   TLV 0x18: Call domain u64, 1 = CS, 3 = IMS (VoLTE)
     *   TLV 0x19: u64 (0), 0x22 u8 (0), 0x23 u32 (0) — vendor defaults
     * There is no 0x11 TLV in the stock frame.
     */
    uint32_t call_type = 1;
    uint8_t domain8[8] = { (uint8_t)(is_ims ? 3 : 1), 0, 0, 0, 0, 0, 0, 0 };
    uint8_t zero8[8] = { 0 };
    uint8_t zero4[4] = { 0 };
    uint16_t l = 0;
    const uint8_t *v;
    int rc;

    if (!number || !number[0])
        return -EINVAL;
    voice_path_set(1);
    voice_path_wait(2500);

    n = qmi_put_tlv(req, n, 0x01, number, (uint16_t)strlen(number));
    n = qmi_put_u8(req, n, 0x10, clir_v);
    n = qmi_put_u32(req, n, 0x16, call_type);
    n = qmi_put_tlv(req, n, 0x18, domain8, sizeof(domain8));
    n = qmi_put_tlv(req, n, 0x19, zero8, sizeof(zero8));
    n = qmi_put_u8(req, n, 0x22, 0);
    n = qmi_put_tlv(req, n, 0x23, zero4, sizeof(zero4));

    rc = qmi_transact(&g_voice, VOICE_DIAL_CALL, req, n, rsp, &rl, 10000);
    if (rc) {
        ALOGE("VOICE: dial rejected %d (call_type=%u domain=%u emergency=%d)", rc,
              call_type, domain8[0], is_emergency);
        voice_path_set(0);
        pthread_mutex_lock(&s_calls_mu);
        s_last_call_fail_cause = 1; /* CALL_FAIL_UNOBTAINABLE_NUMBER */
        pthread_mutex_unlock(&s_calls_mu);
        return rc > 0 ? -EIO : rc;
    }
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (!v)
        v = qmi_find_tlv(rsp, rl, 0x01, &l);
    *call_id = (v && l >= 1) ? v[0] : 1;

    pthread_mutex_lock(&s_calls_mu);
    s_last_call_fail_cause = 16; /* Normal clearing */
    for (int s = 0; s < MAX_CALLS; s++)
        if (!s_calls[s].active) {
            memset(&s_calls[s], 0, sizeof(s_calls[s]));
            s_calls[s].active = 1;
            s_calls[s].id = *call_id;
            s_calls[s].state = 1;
            s_calls[s].direction = 1;
            strncpy(s_calls[s].number, number, sizeof(s_calls[s].number) - 1);
            break;
        }
    pthread_mutex_unlock(&s_calls_mu);
    ALOGI("VOICE: dial ok, id %d (call_type=%u, domain=%u)", *call_id,
          call_type, domain8[0]);
    return 0;
}

int voice_dial(const char *number, int clir, int *call_id)
{
    return voice_dial_internal(number, clir, false, call_id);
}

int voice_dial_emergency(const char *number, int clir, int *call_id)
{
    return voice_dial_internal(number, clir, true, call_id);
}

int voice_end(int call_id)
{
    uint8_t req[16];
    size_t n;
    int rc, s;

    if (call_id <= 0)
        return -EINVAL;
    n = qmi_put_u8(req, 0, 0x01, (uint8_t)call_id);
    /* Stock includes the end reason (2 = normal) in TLV 0x10. */
    n = qmi_put_u32(req, n, 0x10, 2);
    rc = qmi_transact(&g_voice, VOICE_END_CALL, req, n, NULL, NULL, 5000);

    /* The framework already ended this call: drop it locally even if the
     * modem rejects the request, or GET_CURRENT_CALLS keeps it alive. */
    pthread_mutex_lock(&s_calls_mu);
    for (s = 0; s < MAX_CALLS; s++) {
        if (s_calls[s].active && s_calls[s].id == call_id) {
            s_calls[s].active = 0;
            s_calls[s].state = 0;
        }
    }
    pthread_mutex_unlock(&s_calls_mu);
    voice_path_set(0);
    kebab_unsol(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
    return rc;
}

int voice_answer(int call_id)
{
    uint8_t req[8];
    size_t n;

    if (call_id <= 0) {
        /* no argument in the AIDL API: answer the first ringing call */
        pthread_mutex_lock(&s_calls_mu);
        for (int s = 0; s < MAX_CALLS; s++)
            if (s_calls[s].active &&
                (s_calls[s].state == 2 || s_calls[s].state == 7)) {
                call_id = s_calls[s].id;
                break;
            }
        pthread_mutex_unlock(&s_calls_mu);
    }
    if (call_id <= 0)
        return -ENOENT;
    n = qmi_put_u8(req, 0, 0x01, (uint8_t)call_id);
    return qmi_transact(&g_voice, VOICE_ANSWER_CALL, req, n, NULL, NULL, 5000);
}

int voice_dtmf(int call_id, const char *digits)
{
    uint8_t req[32], data[16];
    size_t n, dl = 0;

    if (call_id <= 0) {
        pthread_mutex_lock(&s_calls_mu);
        for (int s = 0; s < MAX_CALLS; s++)
            if (s_calls[s].active && s_calls[s].state == 3) {
                call_id = s_calls[s].id;
                break;
            }
        pthread_mutex_unlock(&s_calls_mu);
    }
    if (!digits || call_id <= 0 || strlen(digits) + 3 > sizeof(data))
        return -EINVAL;
    /* Upstream QMI IDL: TLV 0x01 = {u8 call id, u8 length, digit bytes}. */
    data[dl++] = (uint8_t)call_id;
    data[dl++] = (uint8_t)strlen(digits);
    memcpy(data + dl, digits, strlen(digits));
    dl += strlen(digits);
    n = qmi_put_tlv(req, 0, 0x01, data, (uint16_t)dl);
    return qmi_transact(&g_voice, VOICE_BURST_DTMF, req, n, NULL, NULL, 5000);
}

/* GSM 7-bit default alphabet (3GPP TS 23.038), unpaid table for UI text. */
static const uint16_t gsm7_table[128] = {
    0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec,
    0x00f2, 0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5,
    0x0394, 0x005f, 0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8,
    0x03a3, 0x0398, 0x039e, 0x00a0, 0x00c6, 0x00e6, 0x00df, 0x00c9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00a4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f,
    0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7,
    0x00bf, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1, 0x00fc, 0x00e0,
};

static size_t utf8_put(char *out, size_t o, size_t max, uint16_t cp)
{
    if (cp < 0x80) {
        if (o + 1 < max)
            out[o++] = (char)cp;
    } else {
        if (o + 2 < max) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    return o;
}

static void uss_gsm7(const uint8_t *d, size_t n, char *out, size_t max)
{
    size_t o = 0;
    unsigned bit = 0;
    int skip = 0;

    while (bit + 7 <= n * 8 && o + 2 < max) {
        unsigned idx = 0;

        for (unsigned k = 0; k < 7; k++) {
            unsigned bp = bit + k;

            if (d[bp / 8] & (1u << (bp % 8)))
                idx |= 1u << k;
        }
        bit += 7;
        if (skip) {
            skip = 0;
            continue;
        }
        if (idx == 0x1b) { /* extension escape: drop the next septet */
            skip = 1;
            continue;
        }
        o = utf8_put(out, o, max, gsm7_table[idx & 0x7f]);
    }
    out[o < max ? o : max - 1] = '\0';
}

static void uss_ucs2(const uint8_t *d, size_t n, char *out, size_t max)
{
    size_t o = 0;

    for (size_t i = 0; i + 1 < n && o + 2 < max; i += 2) {
        uint16_t cp = (uint16_t)(d[i] | (d[i + 1] << 8));

        if (!cp)
            break;
        o = utf8_put(out, o, max, cp);
    }
    out[o < max ? o : max - 1] = '\0';
}

/* QMI USS Data Coding Scheme, as used by stock qcril: 1 ascii, 2 GSM 7-bit,
 * 3 UCS-2. Anything else is passed through as UTF-8. */
static void uss_to_utf8(uint8_t dcs, const uint8_t *d, size_t n, char *out,
                        size_t max)
{
    size_t l;

    if (!max)
        return;
    if (dcs == 2) {
        uss_gsm7(d, n, out, max);
        return;
    }
    if (dcs == 3) {
        uss_ucs2(d, n, out, max);
        return;
    }
    l = n < max - 1 ? n : max - 1;
    memcpy(out, d, l);
    while (l && out[l - 1] == '\0')
        l--;
    out[l] = '\0';
}

/* USS Data UTF16 TLV: u8 byte-length prefix + little-endian u16 code units. */
static void uss_utf16_tlv(const uint8_t *v, uint16_t l, char *out, size_t max)
{
    size_t n = 0;

    if (l >= 1) {
        n = v[0];
        if (n + 1 > l)
            n = l - 1;
        uss_ucs2(v + 1, n, out, max);
    } else {
        out[0] = '\0';
    }
}

int voice_ussd(const char *text, int dcs)
{
    uint8_t req[300], rsp[1024], data[256];
    size_t n, dl = 0, rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;
    int rc, len;

    if (!text || !text[0])
        return -EINVAL;
    len = (int)strlen(text);
    if (len > 180)
        len = 180;
    data[dl++] = (uint8_t)dcs;
    data[dl++] = (uint8_t)len;
    memcpy(data + dl, text, (size_t)len);
    dl += (size_t)len;
    n = qmi_put_tlv(req, 0, 0x01, data, (uint16_t)dl);

    rc = qmi_transact(&g_voice, 0x003A /* Originate USSD */, req, n, rsp, &rl,
                      15000);
    if (rc) {
        ALOGE("VOICE: USSD originate failed %d", rc);
        return rc > 0 ? -EIO : rc;
    }
    v = qmi_find_tlv(rsp, rl, 0x12, &l); /* USS Data reply */
    if (v && l >= 2) {
        char text[400];
        size_t sl = v[1];
        char *payload[2];

        if (sl > l - 2)
            sl = l - 2;
        uss_to_utf8(v[0], v + 2, sl, text, sizeof(text));
        payload[0] = (char *)"0"; /* USSD-Notify, no further action */
        payload[1] = text;
        kebab_unsol(RIL_UNSOL_ON_USSD, payload, sizeof(payload));
    } else {
        v = qmi_find_tlv(rsp, rl, 0x16, &l); /* USS Data UTF16 */
        if (v) {
            char text[400];
            char *payload[2];

            uss_utf16_tlv(v, l, text, sizeof(text));
            payload[0] = (char *)"0";
            payload[1] = text;
            kebab_unsol(RIL_UNSOL_ON_USSD, payload, sizeof(payload));
        }
    }
    ALOGI("VOICE: USSD sent");
    return 0;
}

int voice_ussd_cancel(void)
{
    /* Stock completes the cancel request without an unsolicited USSD event;
     * the network's own release indication (0x3D) closes the session. */
    return qmi_transact(&g_voice, 0x003C /* Cancel USSD */, NULL, 0, NULL,
                        NULL, 5000);
}

int voice_get_calls(RIL_Call ***out)
{
    int n = 0;
    RIL_Call **arr;

    pthread_mutex_lock(&s_calls_mu);
    arr = calloc(MAX_CALLS, sizeof(*arr));
    if (!arr) {
        pthread_mutex_unlock(&s_calls_mu);
        *out = NULL;
        return 0;
    }
    for (int s = 0; s < MAX_CALLS; s++) {
        int rs;
        RIL_Call *c;

        if (!s_calls[s].active)
            continue;
        rs = qmi_state_to_ril(s_calls[s].state);
        if (rs < 0)
            continue;
        c = calloc(1, sizeof(*c));
        if (!c)
            continue;
        c->state = (RIL_CallState)rs;
        c->index = s_calls[s].id;
        c->toa = 129;
        c->isMpty = s_calls[s].is_mpty ? 1 : 0;
        c->isMT = (s_calls[s].direction == 2);
        c->als = (char)s_calls[s].als;
        c->isVoice = 1;
        c->number = strdup(s_calls[s].number[0] ? s_calls[s].number : "");
        c->numberPresentation = s_calls[s].number_pres;
        c->name = strdup("");
        c->namePresentation = 0;
        c->uusInfo = NULL;
        arr[n++] = c;
    }
    pthread_mutex_unlock(&s_calls_mu);
    *out = arr;
    return n;
}
