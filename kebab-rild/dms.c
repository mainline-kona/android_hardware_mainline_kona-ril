/*
 * dms.c — DMS operating mode.
 *
 * 0x2E Set Operating Mode: TLV 0x01 mode u8, 0 = online, 1 = low power.
 * Stock never sends mode 3 (offline); QMI error 52 is mapped to success
 * (libril_full.c#L232761).
 */
#define LOG_TAG "kebab-rild"

#include "dms.h"
#include "nas.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#define DMS_SET_OPERATING_MODE 0x002E
#define DMS_GET_IDS            0x0025
#define DMS_GET_REVISION       0x0023
#define QMI_ERR_DEVICE_UNSUPPORTED 52

static void dms_copy_tlv_string(const uint8_t *tlvs, size_t len, uint8_t tlv,
                                char *out, size_t max)
{
    uint16_t l = 0;
    const uint8_t *v = qmi_find_tlv(tlvs, len, tlv, &l);

    out[0] = '\0';
    if (!v || !l)
        return;
    {
        size_t sl = strnlen((const char *)v, l);

        if (sl >= max)
            sl = max - 1;
        memcpy(out, v, sl);
        out[sl] = '\0';
    }
}

int dms_get_ids(struct dms_ids *out)
{
    uint8_t rsp[512];
    size_t rl = sizeof(rsp);

    memset(out, 0, sizeof(*out));
    if (!qmi_client_is_up(&g_dms) && qmi_client_wait_up(&g_dms, 15000))
        return -ENOTCONN;
    if (qmi_transact(&g_dms, DMS_GET_IDS, NULL, 0, rsp, &rl, 10000))
        return -EIO;
    dms_copy_tlv_string(rsp, rl, 0x11, out->imei, sizeof(out->imei));
    dms_copy_tlv_string(rsp, rl, 0x13, out->imeisv, sizeof(out->imeisv));
    dms_copy_tlv_string(rsp, rl, 0x12, out->meid, sizeof(out->meid));
    dms_copy_tlv_string(rsp, rl, 0x10, out->esn, sizeof(out->esn));
    /* The SDX55 returns the SVN (two digits) in TLV 0x13, not a 16-digit
     * IMEISV. Compose it from the first 14 IMEI digits, as Android expects. */
    if (strlen(out->imei) >= 14 && strlen(out->imeisv) == 2) {
        char full[sizeof(out->imeisv)];

        snprintf(full, sizeof(full), "%.14s%s", out->imei, out->imeisv);
        strncpy(out->imeisv, full, sizeof(out->imeisv) - 1);
    }
    return out->imei[0] ? 0 : -ENODATA;
}

int dms_get_revision(char *out, size_t max)
{
    uint8_t rsp[256];
    size_t rl = sizeof(rsp);

    out[0] = '\0';
    if (!qmi_client_is_up(&g_dms) && qmi_client_wait_up(&g_dms, 15000))
        return -ENOTCONN;
    if (qmi_transact(&g_dms, DMS_GET_REVISION, NULL, 0, rsp, &rl, 10000))
        return -EIO;
    dms_copy_tlv_string(rsp, rl, 0x01, out, max);
    return out[0] ? 0 : -ENODATA;
}

/* The modem boots online. Re-apply the framework's intent: online when radio
 * is on, low power when it is off (airplane mode must survive an SSR). */
static void *dms_reonline_thread(void *arg)
{
    bool on;
    int rc = -ENOTCONN;

    (void)arg;
    pthread_mutex_lock(&g_state.mu);
    on = g_state.radio_on;
    pthread_mutex_unlock(&g_state.mu);
    for (int i = 0; i < 15 && qmi_client_is_up(&g_dms); i++) {
        rc = dms_set_online(on);
        if (rc == 0)
            break;
        sleep(1);
    }
    if (on && rc == 0) {
        nas_apply_system_selection_retry();
        nas_refresh();
    }
    return NULL;
}

static void dms_up_cb(struct qmi_client *c, bool up)
{
    pthread_t t;

    (void)c;
    ALOGI("DMS: client %s", up ? "up" : "down");
    kebab_modem_state_changed(up);
    state_update_readiness();
    if (up && pthread_create(&t, NULL, dms_reonline_thread, NULL) == 0)
        pthread_detach(t);
}

void dms_init(void)
{
    if (qmi_client_start(&g_dms, QMI_SVC_DMS, NULL, dms_up_cb) != 0)
        ALOGE("DMS: client start failed");
}

int dms_set_online(bool online)
{
    uint8_t req[8];
    size_t n = 0;
    int rc;

    if (!qmi_client_is_up(&g_dms) &&
        qmi_client_wait_up(&g_dms, 15000) != 0)
        return -ENOTCONN;

    n = qmi_put_u8(req, n, 0x01, online ? 0 : 1);
    rc = qmi_transact(&g_dms, DMS_SET_OPERATING_MODE, req, n, NULL, NULL,
                      20000);
    if (rc == QMI_ERR_DEVICE_UNSUPPORTED) {
        ALOGI("DMS: operating mode %d: error 52 treated as success",
              online ? 0 : 1);
        rc = 0;
    }
    if (rc)
        ALOGE("DMS: operating mode %d failed: %d", online ? 0 : 1, rc);
    else
        ALOGI("DMS: operating mode %d", online ? 0 : 1);
    return rc;
}
