/* state.c — shared radio state. */
#define LOG_TAG "kebab-rild"

#include "state.h"
#include "uim.h"
#include "wms.h"

#include <cutils/properties.h>
#include <stdio.h>
#include <string.h>

struct kebab_state g_state;
struct qmi_client g_uim;
struct qmi_client g_dms;
struct qmi_client g_nas;
struct qmi_client g_wms;
struct qmi_client g_voice;
struct qmi_client g_wds;
struct qmi_client g_imsa;

void state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.mu, NULL);
    g_state.card_state = 3;
    g_state.reg_state = 0;
}

void state_update_readiness(void)
{
    bool reg, voice, sms, radio_on;

    pthread_mutex_lock(&g_state.mu);
    reg = g_state.ims_reg == 2;
    voice = g_state.ims_voice_srv >= 2;
    radio_on = g_state.radio_on;
    pthread_mutex_unlock(&g_state.mu);

    sms = reg && wms_ims_transport();
    property_set("vendor.kebab.modem.up",
                 (radio_on && qmi_client_is_up(&g_dms)) ? "1" : "0");
    property_set("vendor.kebab.sim.ready", uim_ready() ? "1" : "0");
    {
        char num[24] = "";

        pthread_mutex_lock(&g_state.mu);
        if (g_state.msisdn_ok)
            snprintf(num, sizeof(num), "%s", g_state.msisdn);
        pthread_mutex_unlock(&g_state.mu);
        property_set("vendor.kebab.sim.number", num);
    }
    property_set("vendor.kebab.ims.registered", reg ? "1" : "0");
    property_set("vendor.kebab.ims.sms_ready", sms ? "1" : "0");
    property_set("vendor.kebab.ims.voice_ready", (reg && voice) ? "1" : "0");
}
