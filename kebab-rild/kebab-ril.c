/*
 * kebab-ril.c — RIL entry points for kebab-rild (M2 slice).
 *
 * Requests implemented here: radio power, SIM status, IMSI, operator,
 * voice/data registration, signal strength, preferred network type.
 * Everything else returns RIL_E_REQUEST_NOT_SUPPORTED until its milestone.
 * No AT path exists in this implementation.
 */
#define LOG_TAG "kebab-rild"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#include "cell.h"
#include "data.h"
#include "dcm.h"
#include "dms.h"
#include "ims.h"
#include "nas.h"
#include "ril_glue.h"
#include "state.h"
#include "uim.h"
#include "voice.h"
#include "wms.h"

#define REG_STATE_LEN 18
#define REG_DATA_STATE_LEN 15

/* Radio access family bitmasks, as in the reference RIL */
#define GSM   (RAF_GSM | RAF_GPRS | RAF_EDGE)
#define WCDMA (RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP | RAF_UMTS)
#define LTE   (RAF_LTE)
#define NR    (RAF_NR)

static const struct RIL_Env *s_rilenv;
static RIL_RadioState s_radio_state = RADIO_STATE_OFF;

#define RIL_onRequestComplete(t, e, response, responselen) \
    s_rilenv->OnRequestComplete(t, e, response, responselen)
#define RIL_onUnsolicitedResponse(a, b, c) \
    s_rilenv->OnUnsolicitedResponse(a, b, c)

static RIL_RadioState currentState(void);
static int onSupports(int requestCode);
static void onCancel(RIL_Token t);
static const char *getVersion(void);
static void onRequest(int request, void *data, size_t datalen, RIL_Token t);

static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion,
};

static int tec_to_ril(int radio_if)
{
    switch (radio_if) {
    case 4: return 1;   /* GSM */
    case 5: return 3;   /* UMTS/WCDMA */
    case 8: return 14;  /* LTE */
    case 9: return 17;  /* TD-SCDMA */
    case 12:            /* QMI 0x0C = 5G NR */
    case 20:
        return 20;      /* RIL NR */
    default: return 0;
    }
}

enum req_op { OP_POWER, OP_SETUP_DATA, OP_DEACTIVATE_DATA, OP_SEND_SMS,
              OP_DIAL, OP_EMERGENCY_DIAL, OP_USSD, OP_SET_PREF, OP_SIM_AUTH };

struct req_task {
    RIL_Token t;
    int op;
    int on, profile, cid;
    int auth;
    char apn[101];
    char proto[8];
    char roam[8];
    char user[65];
    char pass[65];
    char smsc[32];
    char pdu[512];
    char addr[82];
    char ussd[192];
    int clir;
    bool on_ims;
    int auth_context;
    char auth_data[512];
    char auth_aid[96];
};

static void do_radio_power(int on, RIL_Token t);
static void do_setup_data(struct req_task *k);
static void do_deactivate_data(struct req_task *k);
static void do_send_sms(struct req_task *k);
static void do_dial(struct req_task *k);
static void do_emergency_dial(struct req_task *k);
static void do_ussd(struct req_task *k);
static void do_set_pref(RIL_Token t);
static void do_sim_auth(struct req_task *k);

static void *req_task_thread(void *arg)
{
    struct req_task *k = arg;

    switch (k->op) {
    case OP_POWER:
        do_radio_power(k->on, k->t);
        break;
    case OP_SETUP_DATA:
        do_setup_data(k);
        break;
    case OP_DEACTIVATE_DATA:
        do_deactivate_data(k);
        break;
    case OP_SEND_SMS:
        do_send_sms(k);
        break;
    case OP_DIAL:
        do_dial(k);
        break;
    case OP_EMERGENCY_DIAL:
        do_emergency_dial(k);
        break;
    case OP_USSD:
        do_ussd(k);
        break;
    case OP_SET_PREF:
        do_set_pref(k->t);
        break;
    case OP_SIM_AUTH:
        do_sim_auth(k);
        break;
    }
    free(k);
    return NULL;
}

/* Long QMI operations run off the RIL thread: a blocking handler stalls every
 * request queued behind it (data setup can take ~70 s, SMS 30 s, power 20 s). */
static void run_task(struct req_task *k)
{
    pthread_t th;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, req_task_thread, k) != 0)
        req_task_thread(k); /* fallback: run inline */
    pthread_attr_destroy(&attr);
}

static void request_radio_power(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;

    if (!data || datalen < sizeof(int)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_POWER;
    k->on = *(int *)data;
    run_task(k);
}

void kebab_modem_state_changed(bool up)
{
    bool on;

    pthread_mutex_lock(&g_state.mu);
    on = g_state.radio_on;
    pthread_mutex_unlock(&g_state.mu);
    s_radio_state = up ? (on ? RADIO_STATE_ON : RADIO_STATE_OFF)
                       : RADIO_STATE_UNAVAILABLE;
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
}

static int dms_apply_mode(bool on)
{
    int rc = -ENOTCONN;

    for (int i = 0; i < 15; i++) {
        if (!qmi_client_is_up(&g_dms)) {
            sleep(1);
            continue;
        }
        rc = dms_set_online(on);
        if (rc == 0)
            return 0;
        sleep(1);
    }
    return rc;
}

/* Low-power modem no longer provides any of these; stock reports POWER_OFF,
 * empty data list, no calls and unregistered IMS immediately. */
static void radio_offline_cleanup(void)
{
    data_clear_all();
    kebab_unsol(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);
    voice_clear_calls();
    ims_force_unregistered();
    nas_clear_state();
    wms_radio_changed(false);
}

static void radio_online_refresh(void)
{
    nas_register_indications();
    /* NAS may still be initialising: retry instead of dropping the
     * framework's selection preference. */
    nas_apply_system_selection_retry();
    nas_refresh();
    uim_poll_card();
    wms_radio_changed(true);
}

static volatile bool s_radio_retrying;
static pthread_mutex_t s_radio_retry_mu = PTHREAD_MUTEX_INITIALIZER;

/* Stock qcrild keeps retrying the mode transition; do the same so a slow
 * modem does not leave the framework stuck in UNAVAILABLE. */
static void *radio_retry_thread(void *arg)
{
    bool on = (bool)(intptr_t)arg;

    for (int i = 0; i < 120; i++) {
        bool cur;

        pthread_mutex_lock(&g_state.mu);
        cur = g_state.radio_on;
        pthread_mutex_unlock(&g_state.mu);
        if (cur != on)
            break;
        if (qmi_client_is_up(&g_dms) && dms_set_online(on) == 0) {
            if (on)
                radio_online_refresh();
            else
                radio_offline_cleanup();
            s_radio_state = on ? RADIO_STATE_ON : RADIO_STATE_OFF;
            state_update_readiness();
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                      NULL, 0);
            break;
        }
        sleep(5);
    }
    pthread_mutex_lock(&s_radio_retry_mu);
    s_radio_retrying = false;
    pthread_mutex_unlock(&s_radio_retry_mu);
    return NULL;
}

static void do_radio_power(int on, RIL_Token t)
{
    bool onb = on != 0;
    int rc;

    ALOGI("radio power %d", on);
    pthread_mutex_lock(&g_state.mu);
    g_state.radio_on = onb;
    pthread_mutex_unlock(&g_state.mu);

    if (!onb)
        radio_offline_cleanup();

    rc = dms_apply_mode(onb);
    if (rc == 0) {
        if (onb)
            radio_online_refresh();
        s_radio_state = onb ? RADIO_STATE_ON : RADIO_STATE_OFF;
    } else {
        bool spawn = false;

        ALOGE("radio power %d: mode not applied (%d), retrying", on, rc);
        s_radio_state = RADIO_STATE_UNAVAILABLE;
        pthread_mutex_lock(&s_radio_retry_mu);
        if (!s_radio_retrying) {
            s_radio_retrying = true;
            spawn = true;
        }
        pthread_mutex_unlock(&s_radio_retry_mu);
        if (spawn) {
            pthread_t th;

            if (pthread_create(&th, NULL, radio_retry_thread,
                               (void *)(intptr_t)onb) != 0) {
                pthread_mutex_lock(&s_radio_retry_mu);
                s_radio_retrying = false;
                pthread_mutex_unlock(&s_radio_retry_mu);
            } else {
                pthread_detach(th);
            }
        }
    }
    state_update_readiness();
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_shutdown(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;
    data_deactivate_all();
    dms_set_online(false);
    s_radio_state = RADIO_STATE_UNAVAILABLE;
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_get_sim_status(RIL_Token t)
{
    static RIL_CardStatus_v1_5 s_status;
    static char s_iccid[24];
    int card;
    bool iccid_ok, radio_on;
    char iccid[24] = "";
    bool present;

    pthread_mutex_lock(&g_state.mu);
    card = g_state.card_state;
    iccid_ok = g_state.iccid_ok;
    radio_on = g_state.radio_on;
    strncpy(iccid, g_state.iccid, sizeof(iccid) - 1);
    pthread_mutex_unlock(&g_state.mu);

    /* card_state 3 = not read yet: report PRESENT/DETECTED while the radio is
     * on so the framework does not flash "no SIM" during boot. */
    present = card == 1 || (card == 3 && radio_on);

    memset(&s_status, 0, sizeof(s_status));
    s_status.base.base.base.card_state =
        present ? RIL_CARDSTATE_PRESENT : RIL_CARDSTATE_ABSENT;
    s_status.base.base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
    s_status.base.base.base.gsm_umts_subscription_app_index = present ? 0 : -1;
    s_status.base.base.base.cdma_subscription_app_index = -1;
    s_status.base.base.base.ims_subscription_app_index = -1;
    s_status.base.base.base.num_applications = present ? 1 : 0;
    s_status.base.base.physicalSlotId = 0;
    s_status.base.base.atr = NULL;
    s_status.base.base.iccid = NULL;
    s_status.base.eid = "";
    s_iccid[0] = '\0';
    if (present) {
        strncpy(s_iccid, iccid_ok ? iccid : "", sizeof(s_iccid) - 1);
        s_status.base.base.iccid = s_iccid;
        s_status.base.base.base.applications[0].app_type = RIL_APPTYPE_USIM;
        s_status.base.base.base.applications[0].app_state =
            iccid_ok ? RIL_APPSTATE_READY : RIL_APPSTATE_DETECTED;
        s_status.base.base.base.applications[0].perso_substate = RIL_PERSOSUBSTATE_READY;
        s_status.base.base.base.applications[0].pin1_replaced = 0;
        s_status.base.base.base.applications[0].pin1 = RIL_PINSTATE_UNKNOWN;
        s_status.base.base.base.applications[0].pin2 = RIL_PINSTATE_UNKNOWN;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_status, sizeof(s_status));
}

static void request_get_imsi(RIL_Token t)
{
    char imsi[20] = "";
    bool ok;

    pthread_mutex_lock(&g_state.mu);
    ok = g_state.imsi_ok;
    strncpy(imsi, g_state.imsi, sizeof(imsi) - 1);
    pthread_mutex_unlock(&g_state.mu);

    if (!ok) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, imsi, strlen(imsi) + 1);
}

static void request_get_smsc_address(RIL_Token t)
{
    char smsc[32];

    if (!wms_get_smsc(smsc, sizeof(smsc))) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, smsc, strlen(smsc) + 1);
}

static void request_query_ns_mode(RIL_Token t)
{
    char mode[] = "0"; /* automatic */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, mode, sizeof(mode));
}

static void request_sim_auth(void *data, size_t datalen, RIL_Token t)
{
    const RIL_SimAuthentication *a = data;
    struct req_task *k;

    if (!a || datalen < sizeof(*a) || !a->authData ||
        strlen(a->authData) >= sizeof(k->auth_data) ||
        (a->aid && strlen(a->aid) >= sizeof(k->auth_aid))) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_SIM_AUTH;
    k->auth_context = a->authContext;
    snprintf(k->auth_data, sizeof(k->auth_data), "%s", a->authData);
    snprintf(k->auth_aid, sizeof(k->auth_aid), "%s", a->aid ? a->aid : "");
    run_task(k);
}

static void do_sim_auth(struct req_task *k)
{
    static char resp[512];
    RIL_SIM_IO_Response r;
    int sw1 = 0, sw2 = 0;

    memset(&r, 0, sizeof(r));
    if (uim_sim_auth(k->auth_context, k->auth_data, k->auth_aid, resp,
                     sizeof(resp), &sw1, &sw2) != 0) {
        ALOGE("UIM: SIM auth failed");
        RIL_onRequestComplete(k->t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    r.sw1 = sw1;
    r.sw2 = sw2;
    r.simResponse = resp;
    RIL_onRequestComplete(k->t, RIL_E_SUCCESS, &r, sizeof(r));
}

static void request_sim_io(void *data, size_t datalen, RIL_Token t)
{
    const RIL_SIM_IO_v6 *io = data;
    RIL_SIM_IO_Response r;
    static char hex[1025];
    int sw1 = 0x6f, sw2 = 0;

    if (!io || datalen < sizeof(*io) || !io->path) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    if (uim_sim_io(io->command, io->fileid, io->path, io->aidPtr,
                   io->p1, io->p2, io->p3, &sw1, &sw2, hex,
                   sizeof(hex)) != 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    r.sw1 = sw1;
    r.sw2 = sw2;
    r.simResponse = hex;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &r, sizeof(r));
}

static void request_operator(RIL_Token t)
{
    int mcc = 0, mnc = 0, l = 0;
    char plmn[16] = "", lname[40], sname[40], spn[24] = "";
    char *resp[3];

    nas_get_serving(NULL, NULL, NULL, &mcc, &mnc, &l);
    nas_get_op_name(lname, sizeof(lname));
    nas_get_op_short(sname, sizeof(sname));
    if (!lname[0] || !sname[0])
        uim_get_spn(spn, sizeof(spn));
    if (mcc > 0)
        snprintf(plmn, sizeof(plmn), "%03d%0*d", mcc, l > 0 ? l : 3, mnc);
    /* EONS names from the network are authoritative; the RAT suffix is part
     * of the name (e.g. "Jio True5G"), never synthesized here. */
    if (!lname[0])
        snprintf(lname, sizeof(lname), "%s", spn[0] ? spn : plmn);
    if (!sname[0])
        snprintf(sname, sizeof(sname), "%s", spn[0] ? spn : plmn);
    resp[0] = lname;
    resp[1] = sname;
    resp[2] = plmn;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
}

static void set_resp_str(char **slot, const char *value)
{
    free(*slot);
    *slot = strdup(value);
}

static void request_registration(int request, RIL_Token t)
{
    int n = request == RIL_REQUEST_VOICE_REGISTRATION_STATE ?
            REG_STATE_LEN : REG_DATA_STATE_LEN;
    char **resp = calloc(n, sizeof(char *));
    int reg = 0, ps = 0, rif = 0, mcc = 0, mnc = 0, ml = 0;
    bool have;
    int act;

    if (!resp) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    have = nas_get_serving(&reg, &ps, &rif, &mcc, &mnc, &ml);
    act = have ? tec_to_ril(rif) : 0;
    for (int i = 0; i < n; i++)
        resp[i] = strdup("0");

    /* 0 = not registered, 1 = registered, 2 = searching, 3 = denied */
    set_resp_str(&resp[0], have ? (reg == 1 ? "1" : reg == 2 ? "2" :
                                   reg == 3 ? "3" : "0") : "0");
    if (have && reg == 1 && request == RIL_REQUEST_DATA_REGISTRATION_STATE)
        set_resp_str(&resp[0], ps == 1 ? "1" : "2");
    if (have && act) {
        char b[8];

        snprintf(b, sizeof(b), "%d", act);
        set_resp_str(&resp[3], b);
    }

    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        if (mcc > 0) {
            char b[16];

            snprintf(b, sizeof(b), "%d", mcc);
            set_resp_str(&resp[11], b);
            snprintf(b, sizeof(b), "%d", mnc);
            set_resp_str(&resp[12], b);
            snprintf(b, sizeof(b), "%03d%0*d", mcc, ml > 0 ? ml : 3, mnc);
            set_resp_str(&resp[13], b);
        }
        /* EN-DC / 5G NSA availability indicator (string 14) */
        if (act == 14 || act == 19 || act == 20)
            set_resp_str(&resp[14], "1");
    } else if (mcc > 0) {
        char b[16];

        snprintf(b, sizeof(b), "%d", mcc);
        set_resp_str(&resp[15], b);
        snprintf(b, sizeof(b), "%d", mnc);
        set_resp_str(&resp[16], b);
        snprintf(b, sizeof(b), "%03d%0*d", mcc, ml > 0 ? ml : 3, mnc);
        set_resp_str(&resp[17], b);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, n * sizeof(char *));
    for (int i = 0; i < n; i++)
        free(resp[i]);
    free(resp);
}

static void request_signal_strength(RIL_Token t)
{
    RIL_SignalStrength_v12 ss;

    nas_build_signal_strength(&ss);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &ss, sizeof(ss));
}

static void request_preferred_network(int request, void *data, size_t datalen,
                                      RIL_Token t)
{
    if (request == RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE ||
        request == RIL_REQUEST_SET_ALLOWED_NETWORK_TYPES_BITMAP) {
        struct req_task *k = calloc(1, sizeof(*k));

        (void)data;
        (void)datalen;
        if (!k) {
            RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
            return;
        }
        k->t = t;
        k->op = OP_SET_PREF;
        run_task(k);
        return;
    }
    {
        static int net = 33; /* LTE | NR */

        RIL_onRequestComplete(t, RIL_E_SUCCESS, &net, sizeof(net));
    }
}

static void request_baseband_version(RIL_Token t)
{
    char rev[96] = "";

    if (dms_get_revision(rev, sizeof(rev)) != 0)
        snprintf(rev, sizeof(rev), "SDX55");
    RIL_onRequestComplete(t, RIL_E_SUCCESS, rev, strlen(rev) + 1);
}

static void request_device_identity(RIL_Token t)
{
    struct dms_ids ids;
    char *id[4];

    memset(&ids, 0, sizeof(ids));
    dms_get_ids(&ids);
    id[0] = ids.imei;
    id[1] = ids.imeisv;
    id[2] = ids.meid;
    id[3] = ids.esn;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, id, sizeof(id));
}

static void request_imei(RIL_Token t)
{
    struct dms_ids ids;

    memset(&ids, 0, sizeof(ids));
    if (dms_get_ids(&ids) != 0 || !ids.imei[0]) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, ids.imei, strlen(ids.imei) + 1);
}

static void request_imeisv(RIL_Token t)
{
    struct dms_ids ids;

    memset(&ids, 0, sizeof(ids));
    if (dms_get_ids(&ids) != 0 || !ids.imeisv[0]) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, ids.imeisv, strlen(ids.imeisv) + 1);
}

static void request_ims_send_sms(void *data, size_t datalen, RIL_Token t)
{
    const RIL_IMS_SMS_Message *m = data;
    struct req_task *k;

    if (!m || datalen < sizeof(*m) || m->tech != RADIO_TECH_3GPP ||
        !m->message.gsmMessage || !m->message.gsmMessage[0] ||
        !m->message.gsmMessage[1] ||
        strlen(m->message.gsmMessage[0]) >= sizeof(k->smsc) ||
        strlen(m->message.gsmMessage[1]) >= sizeof(k->pdu)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_SEND_SMS;
    k->on_ims = true;
    snprintf(k->smsc, sizeof(k->smsc), "%s", m->message.gsmMessage[0]);
    snprintf(k->pdu, sizeof(k->pdu), "%s", m->message.gsmMessage[1]);
    run_task(k);
}

static void request_send_sms(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;
    const char **args = data;

    if (!args || datalen < 2 * sizeof(char *) || !args[1]) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    if (strlen(args[1]) >= sizeof(k->pdu) ||
        (args[0] && strlen(args[0]) >= sizeof(k->smsc))) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_SEND_SMS;
    if (args[0])
        snprintf(k->smsc, sizeof(k->smsc), "%s", args[0]);
    snprintf(k->pdu, sizeof(k->pdu), "%s", args[1]);
    run_task(k);
}

static void do_send_sms(struct req_task *k)
{
    RIL_SMS_Response resp;
    RIL_Token t = k->t;
    int ref = 0, rp = 0, r;

    memset(&resp, 0, sizeof(resp));
    bool on_ims = true; /* TLV 0x13 is mandatory; without it the modem never answers */

    r = wms_send_sms(k->smsc[0] ? k->smsc : NULL, k->pdu, on_ims, &ref, &rp);
    if (r == 0) {
        resp.messageRef = ref;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &resp, sizeof(resp));
        return;
    }
    if (r == 1) {
        resp.messageRef = -1;
        resp.errorCode = rp;
        RIL_onRequestComplete(t,
            (rp == 38 || rp == 41 || rp == 42 || rp == 47) ?
                RIL_E_SMS_SEND_FAIL_RETRY : RIL_E_GENERIC_FAILURE,
            &resp, sizeof(resp));
        return;
    }
    resp.messageRef = -1;
    RIL_onRequestComplete(t, r == 2 ? RIL_E_SMS_SEND_FAIL_RETRY :
                          RIL_E_GENERIC_FAILURE, &resp, sizeof(resp));
}

static void request_sms_acknowledge(void *data, size_t datalen, RIL_Token t)
{
    int success = 0;

    if (data && datalen >= sizeof(int))
        success = *(int *)data;
    wms_ack_sms(success, 0);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_dial(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;
    RIL_Dial *d = data;

    if (!d || datalen < sizeof(*d) || !d->address ||
        strlen(d->address) >= sizeof(k->addr)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_DIAL;
    k->clir = d->clir;
    snprintf(k->addr, sizeof(k->addr), "%s", d->address);
    run_task(k);
}

static void do_dial(struct req_task *k)
{
    int id = 0;

    RIL_onRequestComplete(k->t, voice_dial(k->addr, k->clir, &id) == 0 ?
                          RIL_E_SUCCESS : RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void request_emergency_dial(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;
    RIL_EmergencyDial *ed = data;

    if (!ed || datalen < sizeof(*ed) || !ed->dialInfo.address ||
        strlen(ed->dialInfo.address) >= sizeof(k->addr)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_EMERGENCY_DIAL;
    k->clir = ed->dialInfo.clir;
    snprintf(k->addr, sizeof(k->addr), "%s", ed->dialInfo.address);
    run_task(k);
}

static void do_emergency_dial(struct req_task *k)
{
    int id = 0;

    RIL_onRequestComplete(k->t, voice_dial_emergency(k->addr, k->clir, &id) == 0 ?
                          RIL_E_SUCCESS : RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void request_last_call_fail_cause(RIL_Token t)
{
    int cause = voice_get_last_fail_cause();
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &cause, sizeof(cause));
}

static void request_cfg_set_pref_modem(void *data, size_t datalen, RIL_Token t)
{
    int *id = data;

    ALOGI("config: preferred data modem %d",
          (id && datalen >= sizeof(int)) ? *id : -1);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_hangup(void *data, size_t datalen, RIL_Token t)
{
    int *call_index = data;

    if (!call_index || datalen < sizeof(int)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    if (*call_index > 0)
        voice_end(*call_index);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_answer(RIL_Token t)
{
    voice_answer(0);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_dtmf(void *data, size_t datalen, RIL_Token t)
{
    if (data && datalen >= 2)
        voice_dtmf(0, data);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_get_calls(RIL_Token t)
{
    RIL_Call **calls = NULL;
    int n = voice_get_calls(&calls);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, calls, n * sizeof(RIL_Call *));
    for (int i = 0; i < n; i++) {
        free(calls[i]->number);
        free(calls[i]->name);
        free(calls[i]);
    }
    free(calls);
}

static void request_setup_data(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;
    const char **args = data;
    size_t nargs = datalen / sizeof(char *);
    const char *apn, *proto = "IP", *roam = "";
    const char *user = "", *pass = "";
    int auth = 0, profile = 0;

    if (!args || nargs < 3) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    apn = args[2] ? args[2] : "";
    profile = args[1] ? atoi(args[1]) : 0;
    if (nargs > 3 && args[3])
        user = args[3];
    if (nargs > 4 && args[4])
        pass = args[4];
    if (nargs > 5 && args[5])
        auth = atoi(args[5]);
    if (nargs > 6 && args[6] && args[6][0])
        proto = args[6];
    if (nargs > 7 && args[7])
        roam = args[7];
    if (strlen(apn) >= sizeof(k->apn) ||
        strlen(proto) >= sizeof(k->proto) ||
        strlen(roam) >= sizeof(k->roam) ||
        strlen(user) >= sizeof(k->user) ||
        strlen(pass) >= sizeof(k->pass)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_SETUP_DATA;
    snprintf(k->apn, sizeof(k->apn), "%s", apn);
    snprintf(k->proto, sizeof(k->proto), "%s", proto);
    snprintf(k->roam, sizeof(k->roam), "%s", roam);
    snprintf(k->user, sizeof(k->user), "%s", user);
    snprintf(k->pass, sizeof(k->pass), "%s", pass);
    k->auth = auth;
    k->profile = profile;
    run_task(k);
}

static void do_setup_data(struct req_task *k)
{
    RIL_Data_Call_Response_v11 r;
    RIL_Token t = k->t;
    int rc;

    memset(&r, 0, sizeof(r));
    rc = data_setup(k->apn, k->proto, k->roam, k->user, k->pass, k->auth,
                    k->profile, &r);
    if (rc != 0) {
        if (rc == 52)
            r.status = PDP_FAIL_SERVICE_OPTION_NOT_SUPPORTED;
        else if (rc == -EINVAL)
            r.status = PDP_FAIL_MISSING_UNKNOWN_APN;
        else
            r.status = PDP_FAIL_ERROR_UNSPECIFIED;
        r.suggestedRetryTime = -1;
        r.cid = -1;
        r.active = 0;
        r.type = "";
        r.ifname = "";
        r.addresses = "";
        r.dnses = "";
        r.gateways = "";
        r.pcscf = "";
        r.mtu = 0;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &r, sizeof(r));
}

static void request_deactivate_data(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;
    const char **args = data;

    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_DEACTIVATE_DATA;
    k->cid = (args && datalen >= sizeof(char *) && args[0]) ? atoi(args[0]) : -1;
    run_task(k);
}

static void do_deactivate_data(struct req_task *k)
{
    if (k->cid >= 0)
        data_deactivate_cid(k->cid);
    else
        data_deactivate_all();
    RIL_onRequestComplete(k->t, RIL_E_SUCCESS, NULL, 0);
}

static void do_set_pref(RIL_Token t)
{
    RIL_onRequestComplete(t, nas_apply_system_selection() == 0 ? RIL_E_SUCCESS :
                          RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void request_data_call_list(RIL_Token t)
{
    RIL_Data_Call_Response_v11 r[DATA_MAX_CALLS];
    int n = data_snapshot_all(r, DATA_MAX_CALLS);

    if (n > 0)
        RIL_onRequestComplete(t, RIL_E_SUCCESS, r,
                              (size_t)n * sizeof(r[0]));
    else
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_set_initial_attach_apn(void *data, size_t datalen,
                                           RIL_Token t)
{
    const char **args = data;

    if (args && datalen >= sizeof(char *) && args[0])
        data_set_initial_attach_apn(args[0]);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_ims_registration(RIL_Token t)
{
    int reply[2];

    reply[0] = ims_reg_status() == 2 ? 1 : 0;
    reply[1] = RADIO_TECH_3GPP; /* 1 = 3GPP format */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, reply, sizeof(reply));
}

static void request_ussd(void *data, size_t datalen, RIL_Token t)
{
    struct req_task *k;

    (void)datalen;
    if (!data || strlen((const char *)data) >= sizeof(k->ussd)) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        return;
    }
    k->t = t;
    k->op = OP_USSD;
    snprintf(k->ussd, sizeof(k->ussd), "%s", (const char *)data);
    run_task(k);
}

static void do_ussd(struct req_task *k)
{
    RIL_onRequestComplete(k->t, voice_ussd(k->ussd, 0) == 0 ? RIL_E_SUCCESS :
                          RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void request_cancel_ussd(RIL_Token t)
{
    voice_ussd_cancel();
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void request_get_radio_capability(RIL_Token t)
{
    RIL_RadioCapability rc;

    memset(&rc, 0, sizeof(rc));
    rc.version = RIL_RADIO_CAPABILITY_VERSION;
    rc.rat = NR | LTE | WCDMA | GSM;
    strncpy(rc.logicalModemUuid, "com.android.imsstack.modem", MAX_UUID_LENGTH);
    rc.status = RC_STATUS_SUCCESS;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &rc, sizeof(rc));
}

static void request_set_radio_capability(void *data, size_t datalen, RIL_Token t)
{
    RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
}

static void request_hardware_config(RIL_Token t)
{
    RIL_HardwareConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.type = -1;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &cfg, sizeof(cfg));
}

static void request_cell_info_list(RIL_Token t)
{
    RIL_CellInfo_v12 *ci = NULL;
    int n = cell_get_info_list(&ci);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ci, (size_t)n * sizeof(*ci));
    free(ci);
}

static void request_network_scan(RIL_Token t)
{
    int rc = cell_start_network_scan();

    RIL_onRequestComplete(t, rc == 0 ? RIL_E_SUCCESS : RIL_E_GENERIC_FAILURE,
                          NULL, 0);
}

static void onRequest(int request, void *data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_RADIO_POWER:
        request_radio_power(data, datalen, t);
        break;
    case RIL_REQUEST_SHUTDOWN:
        request_shutdown(data, datalen, t);
        break;
    case RIL_REQUEST_GET_SIM_STATUS:
        request_get_sim_status(t);
        break;
    case RIL_REQUEST_GET_IMSI:
        request_get_imsi(t);
        break;
    case RIL_REQUEST_OPERATOR:
        request_operator(t);
        break;
    case RIL_REQUEST_SIM_IO:
        request_sim_io(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_AUTHENTICATION:
        request_sim_auth(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        request_query_ns_mode(t);
        break;
    case RIL_REQUEST_GET_SMSC_ADDRESS:
        request_get_smsc_address(t);
        break;
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        request_registration(request, t);
        break;
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
        request_registration(request, t);
        break;
    case RIL_REQUEST_SIGNAL_STRENGTH:
        request_signal_strength(t);
        break;
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_SET_ALLOWED_NETWORK_TYPES_BITMAP:
    case RIL_REQUEST_GET_ALLOWED_NETWORK_TYPES_BITMAP:
        request_preferred_network(request, data, datalen, t);
        break;
    case RIL_REQUEST_BASEBAND_VERSION:
        request_baseband_version(t);
        break;
    case RIL_REQUEST_DEVICE_IDENTITY:
        request_device_identity(t);
        break;
    case RIL_REQUEST_GET_IMEI:
        request_imei(t);
        break;
    case RIL_REQUEST_GET_IMEISV:
        request_imeisv(t);
        break;
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        request_send_sms(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_SEND_SMS:
        request_ims_send_sms(data, datalen, t);
        break;
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
        request_sms_acknowledge(data, datalen, t);
        break;
    case RIL_REQUEST_DIAL:
        request_dial(data, datalen, t);
        break;
    case RIL_REQUEST_EMERGENCY_DIAL:
        request_emergency_dial(data, datalen, t);
        break;
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        request_last_call_fail_cause(t);
        break;
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_DATA_PROFILE:
    case RIL_REQUEST_SCREEN_STATE:
    case RIL_REQUEST_SET_LOCATION_UPDATES:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_HANGUP:
        request_hangup(data, datalen, t);
        break;
    case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM:
        request_cfg_set_pref_modem(data, datalen, t);
        break;
    case RIL_REQUEST_ANSWER:
        request_answer(t);
        break;
    case RIL_REQUEST_DTMF:
        request_dtmf(data, datalen, t);
        break;
    case RIL_REQUEST_GET_CURRENT_CALLS:
        request_get_calls(t);
        break;
    case RIL_REQUEST_SETUP_DATA_CALL:
        request_setup_data(data, datalen, t);
        break;
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        request_deactivate_data(data, datalen, t);
        break;
    case RIL_REQUEST_DATA_CALL_LIST:
        request_data_call_list(t);
        break;
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
        request_set_initial_attach_apn(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_REGISTRATION_STATE:
        request_ims_registration(t);
        break;
    case RIL_REQUEST_SEND_USSD:
        request_ussd(data, datalen, t);
        break;
    case RIL_REQUEST_CANCEL_USSD:
        request_cancel_ussd(t);
        break;
    case RIL_REQUEST_GET_RADIO_CAPABILITY:
        request_get_radio_capability(t);
        break;
    case RIL_REQUEST_SET_RADIO_CAPABILITY:
        request_set_radio_capability(data, datalen, t);
        break;
    case RIL_REQUEST_GET_HARDWARE_CONFIG:
        request_hardware_config(t);
        break;
    case RIL_REQUEST_START_NETWORK_SCAN:
        request_network_scan(t);
        break;
    case RIL_REQUEST_GET_CELL_INFO_LIST:
        request_cell_info_list(t);
        break;
    default:
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }
}

static RIL_RadioState currentState(void)
{
    return s_radio_state;
}

static int onSupports(int requestCode)
{
    switch (requestCode) {
    case RIL_REQUEST_RADIO_POWER:
    case RIL_REQUEST_SHUTDOWN:
    case RIL_REQUEST_GET_SIM_STATUS:
    case RIL_REQUEST_GET_IMSI:
    case RIL_REQUEST_OPERATOR:
    case RIL_REQUEST_SIM_IO:
    case RIL_REQUEST_SIM_AUTHENTICATION:
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
    case RIL_REQUEST_GET_SMSC_ADDRESS:
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
    case RIL_REQUEST_SIGNAL_STRENGTH:
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_BASEBAND_VERSION:
    case RIL_REQUEST_DEVICE_IDENTITY:
    case RIL_REQUEST_GET_IMEI:
    case RIL_REQUEST_GET_IMEISV:
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
    case RIL_REQUEST_IMS_SEND_SMS:
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
    case RIL_REQUEST_DIAL:
    case RIL_REQUEST_EMERGENCY_DIAL:
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_DATA_PROFILE:
    case RIL_REQUEST_SCREEN_STATE:
    case RIL_REQUEST_SET_LOCATION_UPDATES:
    case RIL_REQUEST_HANGUP:
    case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM:
    case RIL_REQUEST_ANSWER:
    case RIL_REQUEST_DTMF:
    case RIL_REQUEST_GET_CURRENT_CALLS:
    case RIL_REQUEST_SETUP_DATA_CALL:
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
    case RIL_REQUEST_DATA_CALL_LIST:
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
    case RIL_REQUEST_IMS_REGISTRATION_STATE:
    case RIL_REQUEST_SEND_USSD:
    case RIL_REQUEST_CANCEL_USSD:
    case RIL_REQUEST_GET_RADIO_CAPABILITY:
    case RIL_REQUEST_SET_RADIO_CAPABILITY:
    case RIL_REQUEST_GET_HARDWARE_CONFIG:
    case RIL_REQUEST_START_NETWORK_SCAN:
    case RIL_REQUEST_GET_CELL_INFO_LIST:
        return 1;
    default:
        return 0;
    }
}

static void onCancel(RIL_Token t)
{
    (void)t;
}

void kebab_unsol(int response, const void *data, size_t datalen)
{
    s_rilenv->OnUnsolicitedResponse(response, data, datalen);
}

static const char *getVersion(void)
{
    return "kebab-rild 0.1 (libqmi plan, socket transport)";
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc,
                                   char **argv)
{
    (void)argc;
    (void)argv;

    s_rilenv = env;
    state_init();
    uim_init();
    dms_init();
    nas_init();
    wms_init();
    voice_init();
    data_init();
    ims_init();
    dcm_init();
    ALOGI("kebab-rild initialised");
    return &s_callbacks;
}
