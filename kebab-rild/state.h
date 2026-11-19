/* state.h — shared radio state for kebab-rild modules. */
#ifndef KEBAB_STATE_H
#define KEBAB_STATE_H

#include <pthread.h>
#include <stdbool.h>

#include "qmi/qmi.h"

struct kebab_state {
    pthread_mutex_t mu;

    bool radio_on;

    /* UIM */
    int card_state;          /* QMI: 0 absent, 1 present, 2 error, 3 unknown */
    bool iccid_ok;
    bool imsi_ok;
    char iccid[24];
    char imsi[20];
    bool spn_ok;
    char spn[24];
    bool msisdn_ok;
    char msisdn[24];

    /* NAS */
    bool serving_ok;
    int reg_state;           /* 0 not reg, 1 registered, 2 searching, 3 denied */
    int ps_state;
    int radio_if;
    bool roaming;
    int mcc, mnc, mnc_len;
    char op_name[40];        /* EONS long name, e.g. "Jio True5G" */
    char op_short[40];       /* EONS short name */
    bool eons_ok;

    /* signal */
    bool signal_ok;
    int rssi, rsrp, rsrq, snr;
    int sig_rat;             /* 1 = LTE (0x14), 2 = NR (0x17) */

    /* IMS */
    int ims_reg;        /* QMI IMSA: 0 not registered, 1 registering, 2 registered */
    int ims_voice_srv;  /* 0x0021 TLV 0x11: 0 unavailable, 1 limited, 2 available */
};

extern struct kebab_state g_state;
extern struct qmi_client g_uim;
extern struct qmi_client g_dms;
extern struct qmi_client g_nas;
extern struct qmi_client g_wms;
extern struct qmi_client g_voice;
extern struct qmi_client g_wds;
extern struct qmi_client g_imsa;

void state_init(void);
void state_update_readiness(void);

#endif /* KEBAB_STATE_H */
