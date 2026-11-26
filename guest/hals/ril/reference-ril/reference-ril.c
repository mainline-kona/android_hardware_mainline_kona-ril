/* //device/system/reference-ril/reference-ril.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <telephony/ril_cdma_sms.h>
#include <telephony/librilutils.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include "atchannel.h"
#include "at_tok.h"
#include "base64util.h"
#include "misc.h"
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/qrtr.h>
#include <linux/vm_sockets.h>
#include <arpa/inet.h>
#include <limits.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#include "guest/hals/ril/reference-libril/ril.h"
#define LOG_TAG "RIL"
#include <utils/Log.h>

static void *noopRemoveWarning( void *a ) { return a; }
#define RIL_UNUSED_PARM(a) noopRemoveWarning((void *)&(a));

#define MAX_AT_RESPONSE 0x1000

#define MAX_PDP 11  // max LTE bearers

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
// This is used if Wifi is not supported, plain old eth0
#ifdef CUTTLEFISH_ENABLE
#define PPP_TTY_PATH_ETH0 "buried_eth0"
#else
#define PPP_TTY_PATH_ETH0 "eth0"
#endif
// This is used for emulator
#define EMULATOR_RADIO_INTERFACE "eth0"

// for sim
#define AUTH_CONTEXT_EAP_SIM                    128
#define AUTH_CONTEXT_EAP_AKA                    129
#define SIM_AUTH_RESPONSE_SUCCESS               0
#define SIM_AUTH_RESPONSE_SYNC_FAILURE          3

// Default MTU value
#define DEFAULT_MTU 1500

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some variants of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

/* Modem Technology bits */
#define MDM_GSM         0x01
#define MDM_WCDMA       0x02
#define MDM_CDMA        0x04
#define MDM_EVDO        0x08
#define MDM_TDSCDMA     0x10
#define MDM_LTE         0x20
#define MDM_NR          0x40

typedef struct {
    int supportedTechs; // Bitmask of supported Modem Technology bits
    int currentTech;    // Technology the modem is currently using (in the format used by modem)
    int isMultimode;

    // Preferred mode bitmask. This is actually 4 byte-sized bitmasks with different priority values,
    // in which the byte number from LSB to MSB give the priority.
    //
    //          |MSB|   |   |LSB
    // value:   |00 |00 |00 |00
    // byte #:  |3  |2  |1  |0
    //
    // Higher byte order give higher priority. Thus, a value of 0x0000000f represents
    // a preferred mode of GSM, WCDMA, CDMA, and EvDo in which all are equally preferable, whereas
    // 0x00000201 represents a mode with GSM and WCDMA, in which WCDMA is preferred over GSM
    int32_t preferredNetworkMode;
    int subscription_source;

} ModemInfo;

static ModemInfo *sMdmInfo;
// TECH returns the current technology in the format used by the modem.
// It can be used as an l-value
#define TECH(mdminfo)                 ((mdminfo)->currentTech)
// TECH_BIT returns the bitmask equivalent of the current tech
#define TECH_BIT(mdminfo)            (1 << ((mdminfo)->currentTech))
#define IS_MULTIMODE(mdminfo)         ((mdminfo)->isMultimode)
#define TECH_SUPPORTED(mdminfo, tech) ((mdminfo)->supportedTechs & (tech))
#define PREFERRED_NETWORK(mdminfo)    ((mdminfo)->preferredNetworkMode)
// CDMA Subscription Source
#define SSOURCE(mdminfo)              ((mdminfo)->subscription_source)

static int net2modem[] = {
    MDM_GSM | MDM_WCDMA,                                 // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
    MDM_LTE | MDM_WCDMA,                                 // 12 - LTE and WCDMA
    MDM_TDSCDMA,                                         // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA,                             // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA,                               // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM,                               // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM,                     // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,                   // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA,                   // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,         // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,            // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR,                                                             // 23 - NR 5G only mode
    MDM_NR | MDM_LTE,                                                   // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO,                             // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM,                             // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,       // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA,                                       // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA,                                     // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM,                           // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA,                         // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM,               // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int32_t net2pmask[] = {
    MDM_GSM | (MDM_WCDMA << 8),                          // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
    MDM_LTE | MDM_WCDMA,                                 // 12 - LTE and WCDMA
    MDM_TDSCDMA,                                         // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA,                             // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA,                               // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM,                               // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM,                     // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,                   // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA,                   // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,         // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,            // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR,                                                             // 23 - NR 5G only mode
    MDM_NR | MDM_LTE,                                                   // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO,                             // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM,                             // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,       // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA,                                       // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA,                                     // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM,                           // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA,                         // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM,               // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

#define GSM   (RAF_GSM | RAF_GPRS | RAF_EDGE)
#define CDMA  (RAF_IS95A | RAF_IS95B | RAF_1xRTT)
#define EVDO  (RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD)
#define WCDMA (RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP | RAF_UMTS)
#define LTE   (RAF_LTE)
#define NR    (RAF_NR)

typedef struct {
    int bitmap;
    int type;
} NetworkTypeBitmap;

static NetworkTypeBitmap s_networkMask[] = {
    {WCDMA | GSM,                     MDM_GSM | (MDM_WCDMA << 8)},                // 0 - GSM / WCDMA Pref
    {GSM,                             MDM_GSM},                                   // 1 - GSM only
    {WCDMA,                           MDM_WCDMA},                                 // 2 - WCDMA only
    {WCDMA | GSM,                     MDM_GSM | MDM_WCDMA},                       // 3 - GSM / WCDMA Auto
    {CDMA | EVDO,                     MDM_CDMA | MDM_EVDO},                       // 4 - CDMA / EvDo Auto
    {CDMA,                            MDM_CDMA},                                  // 5 - CDMA only
    {EVDO,                            MDM_EVDO},                                  // 6 - EvDo only
    {GSM | WCDMA | CDMA | EVDO,       MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO}, // 7 - GSM/WCDMA, CDMA, EvDo
    {LTE | CDMA | EVDO,               MDM_LTE | MDM_CDMA | MDM_EVDO},             // 8 - LTE, CDMA and EvDo
    {LTE | GSM | WCDMA,               MDM_LTE | MDM_GSM | MDM_WCDMA},             // 9 - LTE, GSM/WCDMA
    {LTE | CDMA | EVDO | GSM | WCDMA, MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA}, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    {LTE,                             MDM_LTE},                                             // 11 - LTE only
    {LTE | WCDMA,                     MDM_LTE | MDM_WCDMA},                                 // 12 - LTE and WCDMA
    {RAF_TD_SCDMA,                    MDM_TDSCDMA},                                         // 13 - TD-SCDMA only
    {RAF_TD_SCDMA | WCDMA,            MDM_WCDMA | MDM_TDSCDMA},                             // 14 - TD-SCDMA and WCDMA
    {LTE | RAF_TD_SCDMA,              MDM_LTE | MDM_TDSCDMA},                               // 15 - LTE and TD-SCDMA
    {RAF_TD_SCDMA | GSM,              MDM_TDSCDMA | MDM_GSM},                               // 16 - TD-SCDMA and GSM
    {LTE | RAF_TD_SCDMA | GSM,        MDM_LTE | MDM_TDSCDMA | MDM_GSM},                     // 17 - TD-SCDMA, GSM and LTE
    {RAF_TD_SCDMA | GSM | WCDMA,      MDM_WCDMA | MDM_TDSCDMA | MDM_GSM},                   // 18 - TD-SCDMA, GSM and WCDMA
    {LTE | RAF_TD_SCDMA | WCDMA,      MDM_LTE | MDM_WCDMA | MDM_TDSCDMA},                   // 19 - LTE, TD-SCDMA and WCDMA
    {LTE | RAF_TD_SCDMA | GSM | WCDMA,MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM},         // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    {RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA,  MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM},            // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    {LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA, MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM},  // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    {NR,                              MDM_NR},                                                             // 23 - NR 5G only mode
    {NR | LTE,                        MDM_NR | MDM_LTE},                                                   // 24 - NR 5G, LTE
    {NR | LTE | CDMA | EVDO,          MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO},                             // 25 - NR 5G, LTE, CDMA and EvDo
    {NR | LTE | GSM | WCDMA,          MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM},                             // 26 - NR 5G, LTE, GSM and WCDMA
    {NR | LTE | CDMA | EVDO | GSM | WCDMA, MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM},  // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    {NR | LTE | WCDMA,                MDM_NR | MDM_LTE | MDM_WCDMA},                                       // 28 - NR 5G, LTE and WCDMA
    {NR | LTE | RAF_TD_SCDMA,         MDM_NR | MDM_LTE | MDM_TDSCDMA},                                     // 29 - NR 5G, LTE and TDSCDMA
    {NR | LTE | RAF_TD_SCDMA | GSM,   MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM},                           // 30 - NR 5G, LTE, TD-SCDMA and GSM
    {NR | LTE | RAF_TD_SCDMA | WCDMA, MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA},                         // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    {NR | LTE | RAF_TD_SCDMA | GSM | WCDMA, MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM},         // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    {NR | LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA, MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM},  // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int is3gpp2(int radioTech) {
    switch (radioTech) {
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            return 1;
        default:
            return 0;
    }
}

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2,
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    RUIM_ABSENT = 6,
    RUIM_NOT_READY = 7,
    RUIM_READY = 8,
    RUIM_PIN = 9,
    RUIM_PUK = 10,
    RUIM_NETWORK_PERSONALIZATION = 11,
    ISIM_ABSENT = 12,
    ISIM_NOT_READY = 13,
    ISIM_READY = 14,
    ISIM_PIN = 15,
    ISIM_PUK = 16,
    ISIM_NETWORK_PERSONALIZATION = 17,
} SIM_Status;

#if defined(ANDROID_MULTI_SIM)
static void onRequest (int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
static RIL_RadioState currentState(RIL_SOCKET_ID socket_id);
#else
static void onRequest (int request, void *data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState();
#endif
static int onSupports (int requestCode);
static void onCancel (RIL_Token t);
static const char *getVersion();
static int isRadioOn();
static SIM_Status getSIMStatus();
static int getCardStatus(RIL_CardStatus_v1_5 **pp_card_status);
static void freeCardStatus(RIL_CardStatus_v1_5 *p_card_status);
static void onDataCallListChanged(void *param);
bool areUiccApplicationsEnabled = true;

extern const char * requestToString(int request);
extern uint8_t hexCharToInt(uint8_t c);
extern uint8_t * convertHexStringToBytes(void *response, size_t responseLen);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

#ifdef RIL_SHLIB
static const struct RIL_Env *s_rilenv;

#define RIL_onRequestComplete(t, e, response, responselen) s_rilenv->OnRequestComplete(t,e, response, responselen)
#if defined(ANDROID_MULTI_SIM)
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c,RIL_SOCKET_1)
#define RIL_onUnsolicitedResponseForSlot(a,b,c,s) s_rilenv->OnUnsolicitedResponse(a,b,c,s)
#else
#define RIL_onUnsolicitedResponse(a,b,c) s_rilenv->OnUnsolicitedResponse(a,b,c)
#define RIL_onUnsolicitedResponseForSlot(a,b,c,s) s_rilenv->OnUnsolicitedResponse(a,b,c)
#endif
#define RIL_requestTimedCallback(a,b,c) s_rilenv->RequestTimedCallback(a,b,c)
#endif

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;
static RIL_RadioState sStateSlot2 = RADIO_STATE_UNAVAILABLE;
static int s_slot2PreferredNetworkType = 26;
static int s_slot2AllowedNetworkTypes = NR | LTE | WCDMA | GSM;
static bool isNrDualConnectivityEnabled = true;

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static int s_port = -1;
static const char * s_device_path = NULL;
static int          s_device_socket = 0;
static uint32_t s_modem_simulator_port = -1;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static int sFD;     /* file desc of AT channel */
static char sATBuffer[MAX_AT_RESPONSE+1];
static char *sATBufferCur = NULL;

static const struct timeval TIMEVAL_SIMPOLL = {0, 250000};
static const struct timeval TIMEVAL_CALLSTATEPOLL = {0,500000};
static const struct timeval TIMEVAL_0 = {0,0};
static const struct timeval TIMEVAL_SIGNAL_IMMEDIATE = {0, 200000};
static const struct timeval TIMEVAL_RAT_SETTLE = {1, 500000};

static int s_lastRegTech = RADIO_TECH_UNKNOWN;
static int s_lastRegAct = -1;
static int s_lastSignalValid = 0;
static int s_copsDeregistered = 0;
static int s_lastSignal[sizeof(RIL_SignalStrength_v12) / sizeof(int)];

static void pushSignalStrengthOnce(void *param __unused);
static void notifyNetworkAndSignal(void *param __unused);

static int s_ims_registered  = 0;        // 0==unregistered
static int s_ims_services    = 1;        // & 0x1 == sms over ims supported
static int s_ims_format    = 1;          // FORMAT_3GPP(1) vs FORMAT_3GPP2(2);
static int s_ims_cause_retry = 0;        // 1==causes sms over ims to temp fail
static int s_ims_cause_perm_failure = 0; // 1==causes sms over ims to permanent fail
static int s_ims_gsm_retry   = 0;        // 1==causes sms over gsm to temp fail
static int s_ims_gsm_fail    = 0;        // 1==causes sms over gsm to permanent fail

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */


static int s_cell_info_rate_ms = INT_MAX;
static int s_mcc = 0;
static int s_mnc = 0;
static int s_mncLength = 2;
static int s_lac = 0;
static int s_cid = 0;

// STK
static bool s_stkServiceRunning = false;
static char *s_stkUnsolResponse = NULL;

// Next available handle for keep alive session
static int32_t s_session_handle = 1;

typedef enum {
    STK_UNSOL_EVENT_UNKNOWN,
    STK_UNSOL_EVENT_NOTIFY,
    STK_UNSOL_PROACTIVE_CMD,
} StkUnsolEvent;

typedef enum {
    STK_RUN_AT        = 0x34,
    STK_SEND_DTMF     = 0x14,
    STK_SEND_SMS      = 0x13,
    STK_SEND_SS       = 0x11,
    STK_SEND_USSD     = 0x12,
    STK_PLAY_TONE     = 0x20,
    STK_OPEN_CHANNEL  = 0x40,
    STK_CLOSE_CHANNEL = 0x41,
    STK_RECEIVE_DATA  = 0x42,
    STK_SEND_DATA     = 0x43,
    STK_GET_CHANNEL_STATUS = 0x44,
    STK_REFRESH       = 0x01,
} StkCmdType;

enum PDPState {
    PDP_IDLE,
    PDP_BUSY,
};

struct PDPInfo {
    int cid;
    enum PDPState state;
};

struct PDPInfo s_PDP[] = {
        {1, PDP_IDLE}, {2, PDP_IDLE}, {3, PDP_IDLE}, {4, PDP_IDLE},  {5, PDP_IDLE},  {6, PDP_IDLE},
        {7, PDP_IDLE}, {8, PDP_IDLE}, {9, PDP_IDLE}, {10, PDP_IDLE}, {11, PDP_IDLE},
};

static void pollSIMState (void *param);
static void setRadioState(RIL_RadioState newState);
static void setRadioTechnology(ModemInfo *mdm, int newtech);
static int query_ctec(ModemInfo *mdm, int *current, int32_t *preferred);
static int parse_technology_response(const char *response, int *current, int32_t *preferred);
static int techFromModemType(int mdmtype);
static void getIccId(char *iccid, int size);

static int clccStateToRILState(int state, RIL_CallState *p_state)
{
    switch(state) {
        case 0: *p_state = RIL_CALL_ACTIVE;   return 0;
        case 1: *p_state = RIL_CALL_HOLDING;  return 0;
        case 2: *p_state = RIL_CALL_DIALING;  return 0;
        case 3: *p_state = RIL_CALL_ALERTING; return 0;
        case 4: *p_state = RIL_CALL_INCOMING; return 0;
        case 5: *p_state = RIL_CALL_WAITING;  return 0;
        default: return -1;
    }
}

void convertBytesToHexString(char *bin_ptr, int length, unsigned char *hex_ptr) {
    int i;
    unsigned char tmp;

    if (bin_ptr == NULL || hex_ptr == NULL) {
        return;
    }
    for (i = 0; i < length; i++) {
        tmp = (unsigned char)((bin_ptr[i] & 0xf0) >> 4);
        if (tmp <= 9) {
            *hex_ptr = (unsigned char)(tmp + '0');
        } else {
            *hex_ptr = (unsigned char)(tmp + 'A' - 10);
        }
        hex_ptr++;
        tmp = (unsigned char)(bin_ptr[i] & 0x0f);
        if (tmp <= 9) {
            *hex_ptr = (unsigned char)(tmp + '0');
        } else {
            *hex_ptr = (unsigned char)(tmp + 'A' - 10);
        }
        hex_ptr++;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
        //+CLCC: 1,0,2,0,0,\"+18005551212\",145
        //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0) goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0) return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")
        ) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0) goto error;
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    RLOGE("invalid CLCC line\n");
    return -1;
}

static int parseSimResponseLine(char* line, RIL_SIM_IO_Response* response) {
    int err;

    err = at_tok_start(&line);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw1);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw2);
    if (err < 0) return err;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &response->simResponse);
        if (err < 0) return err;
    }
    return 0;
}

#ifdef CUTTLEFISH_ENABLE
static void set_Ip_Addr(const char *addr, const char* radioInterfaceName) {
  RLOGD("%s %d setting ip addr %s on interface %s", __func__, __LINE__, addr,
        radioInterfaceName);
  struct ifreq request;
  int status = 0;
  int family = strchr(addr, ':') ? AF_INET6 : AF_INET;
  int sock = socket(family, SOCK_DGRAM, 0);
  if (sock == -1) {
    RLOGE("Failed to open interface socket: %s (%d)", strerror(errno), errno);
    return;
  }

  memset(&request, 0, sizeof(request));
  strncpy(request.ifr_name, radioInterfaceName, sizeof(request.ifr_name));
  request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';

  int pfxlen = 0;
  char *myaddr = strdup(addr);
  char *pch = NULL;
  pch = strchr(myaddr, '/');
  if (pch) {
    *pch = '\0';
    pfxlen = atoi(++pch);
  }

  if (family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)&request.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(myaddr);
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
      RLOGE("%s: SIOCSIFADDR IPv4 failed.", __func__);
    }
    sin->sin_addr.s_addr = htonl(0xFFFFFFFFu << (32 - (pfxlen ?: 32)));
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
      RLOGE("%s: SIOCSIFNETMASK failed.", __func__);
    }
  } else {
    if (ioctl(sock, SIOGIFINDEX, &request) < 0) {
      RLOGE("%s: SIOCGIFINDEX failed.", __func__);
    }

    struct in6_ifreq req6 = {
       // struct in6_addr ifr6_addr;
       .ifr6_prefixlen = pfxlen ?: 128,  // __u32
       .ifr6_ifindex = request.ifr_ifindex,  // int
    };
    if (inet_pton(AF_INET6, myaddr, &req6.ifr6_addr) != 1) {
      RLOGE("%s: inet_pton(AF_INET6, '%s') failed.", __func__, myaddr);
    }

    if (ioctl(sock, SIOCSIFADDR, &req6) < 0) {
      RLOGE("%s: SIOCSIFADDR IPv6 failed.", __func__);
    }
  }

  close(sock);
  free(myaddr);
  RLOGD("%s %d done.", __func__, __LINE__);
}
#endif

enum InterfaceState {
    kInterfaceUp,
    kInterfaceDown,
};

static RIL_Errno setInterfaceState(const char* interfaceName,
                                   enum InterfaceState state) {
    struct ifreq request;
    int status = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)",
              strerror(errno), errno);
        return RIL_E_GENERIC_FAILURE;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interfaceName, sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';
    status = ioctl(sock, SIOCGIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to get interface flags for %s: %s (%d)",
              interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_RADIO_NOT_AVAILABLE;
    }

    bool isUp = (request.ifr_flags & IFF_UP);
    if ((state == kInterfaceUp && isUp) || (state == kInterfaceDown && !isUp)) {
        // Interface already in desired state
        close(sock);
        return RIL_E_SUCCESS;
    }

    // Simply toggle the flag since we know it's the opposite of what we want
    request.ifr_flags ^= IFF_UP;

    status = ioctl(sock, SIOCSIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to set interface flags for %s: %s (%d)",
              interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_GENERIC_FAILURE;
    }

    close(sock);
    return RIL_E_SUCCESS;
}

/**
 * Arm unsolicited network registration URCs with full location/AcT info (<n>=2).
 * SDX55 resets +CREG/+CGREG/+CEREG/+C5GREG reporting mode back to 0 across
 * AT+CFUN=0 -> AT+CFUN=1 (Airplane Mode) and can reject early AT init before
 * NAS is ready, so call this on init, radio power-on, and SIM-ready.
 */
static void enableNetworkRegistrationUrks(void)
{
    ATResponse *p_response = NULL;
    int err;

    err = at_send_command("AT+CREG=2", &p_response);
    if (err < 0 || !p_response || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }
    at_response_free(p_response);
    p_response = NULL;

    err = at_send_command("AT+CGREG=2", &p_response);
    if (err < 0 || !p_response || p_response->success == 0) {
        at_send_command("AT+CGREG=1", NULL);
    }
    at_response_free(p_response);
    p_response = NULL;

    err = at_send_command("AT+CEREG=2", &p_response);
    if (err < 0 || !p_response || p_response->success == 0) {
        at_send_command("AT+CEREG=1", NULL);
    }
    at_response_free(p_response);
    p_response = NULL;

    err = at_send_command("AT+C5GREG=2", &p_response);
    if (err < 0 || !p_response || p_response->success == 0) {
        at_send_command("AT+C5GREG=1", NULL);
    }
    at_response_free(p_response);

    at_send_command("AT+CGEREP=1,0", NULL);
}

/** do post-AT+CFUN=1 initialization */
static void onRadioPowerOn()
{
#ifdef USE_TI_COMMANDS
    /*  Must be after CFUN=1 */
    /*  TI specific -- notifications for CPHS things such */
    /*  as CPHS message waiting indicator */

    at_send_command("AT%CPHS=1", NULL);

    /*  TI specific -- enable NITZ unsol notifs */
    at_send_command("AT%CTZV=1", NULL);
#endif

    enableNetworkRegistrationUrks();
    s_lastSignalValid = 0;
    pollSIMState(NULL);
}

/** do post- SIM ready initialization */
static void onSIMReady()
{
    enableNetworkRegistrationUrks();
    if (areUiccApplicationsEnabled && s_copsDeregistered) {
        s_copsDeregistered = 0;
        at_send_command("AT+COPS=0", NULL);
    }
    at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
    RIL_requestTimedCallback(pushSignalStrengthOnce, NULL, &TIMEVAL_SIGNAL_IMMEDIATE);
}

static int useDataconn(void);
static void stopDataconn(int cid);

static void requestRadioPower(void *data, size_t datalen __unused, RIL_Token t)
{
    int onOff;

    int err;
    ATResponse *p_response = NULL;

    assert (datalen >= sizeof(int *));
    onOff = ((int *)data)[0];

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
        if (useDataconn()) {
            stopDataconn(0);
        }
        s_lastRegTech = RADIO_TECH_UNKNOWN;
        s_lastRegAct = -1;
        s_lastSignalValid = 0;
        err = at_send_command("AT+CFUN=0", &p_response);
        if (err < 0 || p_response->success == 0) goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        if (isRadioOn() != 1) {
            err = at_send_command("AT+CFUN=1", &p_response);
            if (err < 0 || p_response->success == 0) {
                // Some stacks return an error when there is no SIM,
                // but they really turn the RF portion on
                // So, if we get an error, let's check to see if it
                // turned on anyway
                if (isRadioOn() != 1) {
                    goto error;
                }
            }
            /*
             * SDX55 returns OK to AT+CFUN=1 ~600-800 ms before its AT/NAS
             * layer stops returning ERROR/+CME ERROR: 14. Wait briefly for
             * AT+CEREG? to succeed before signaling RADIO_STATE_ON so Android
             * does not enter ServiceStateTracker/DNC retry backoffs.
             */
            for (int attempt = 0; attempt < 10; attempt++) {
                ATResponse *probe = NULL;
                int perr = at_send_command_singleline("AT+CEREG?", "+CEREG:", &probe);
                int ok = (perr == 0 && probe && probe->success != 0);
                at_response_free(probe);
                if (ok) break;
                usleep(150000);
            }
        }
        enableNetworkRegistrationUrks();
        setRadioState(RADIO_STATE_ON);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestShutdown(RIL_Token t)
{
    int onOff;

    ATResponse *p_response = NULL;

    if (sState != RADIO_STATE_OFF) {
        at_send_command("AT+CFUN=0", &p_response);
        setRadioState(RADIO_STATE_UNAVAILABLE);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
}

// This is the function that will run on the new background thread.
// This is to simulate the radio state change during reboot.
static void* broadcastRadioStateThreadFunc(void* arg __unused) {
    // Wait 1 second
    sleep(1);

    // Change state to OFF
    setRadioState(RADIO_STATE_OFF);

    // Wait another second
    sleep(1);

    // Change state back to ON
    setRadioState(RADIO_STATE_ON);

    return NULL;
}

static void requestNvResetConfig(void* data, size_t datalen __unused, RIL_Token t) {
    assert(datalen >= sizeof(int*));
    int nvConfig = ((int*)data)[0];
    if (nvConfig == 1 /* ResetNvType::RELOAD */) {
        pthread_t tid;  // Variable to hold the thread ID
        // Create and start the new thread to broadcast radio state change.
        int err = pthread_create(&tid, NULL, &broadcastRadioStateThreadFunc, NULL);
        // Detach the thread to let it run independently and clean up automatically
        pthread_detach(tid);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestOrSendDataCallList(int cid, RIL_Token *t);

static void onDataCallListChanged(void *param __unused)
{
    requestOrSendDataCallList(-1, NULL);
}

static void requestDataCallList(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    requestOrSendDataCallList(-1, &t);
}

// Hang up, reject, conference, call waiting
static void requestCallSelection(
                void *data __unused, size_t datalen __unused, RIL_Token t, int request)
{
    // 3GPP 22.030 6.5.5
    static char hangupWaiting[]    = "AT+CHLD=0";
    static char hangupForeground[] = "AT+CHLD=1";
    static char switchWaiting[]    = "AT+CHLD=2";
    static char conference[]       = "AT+CHLD=3";
    static char reject[]           = "ATH";

    char* atCommand;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch(request) {
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
            // "Releases all held calls or sets User Determined User Busy
            //  (UDUB) for a waiting call."
            atCommand = hangupWaiting;
            break;
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
            // "Releases all active calls (if any exist) and accepts
            //  the other (held or waiting) call."
            atCommand = hangupForeground;
            break;
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
            // "Places all active calls (if any exist) on hold and accepts
            //  the other (held or waiting) call."
            atCommand = switchWaiting;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */
            break;
        case RIL_REQUEST_CONFERENCE:
            // "Adds a held call to the conversation"
            atCommand = conference;
            break;
        case RIL_REQUEST_UDUB:
            // User determined user busy (reject)
            atCommand = reject;
            break;
        default:
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            return;
    }
    at_send_command(atCommand, NULL);
    // Success or failure is ignored by the upper layer here.
    // It will call GET_CURRENT_CALLS and determine success that way.
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static const char* getRadioInterfaceName()
{
    if (isInEmulator()) {
        return EMULATOR_RADIO_INTERFACE;
    }
    return PPP_TTY_PATH_ETH0;
}

/*
 * On a real modem, each data session is set up over QMI by kona-dataconn with a
 * dedicated mux_id (cid 1..MAX_PDP) and held until SIGTERM.
 */
#define DATACONN_PATH "/vendor/bin/kona-dataconn"

struct DataconnSession {
    pid_t pid;
    char type[16];
    char ifname[32];
    char addr[128];
    char gw[128];
    char dns[256];
    char pcscf[256];
    int mtu;
};

static struct DataconnSession s_sessions[MAX_PDP];

static int useDataconn(void) {
    return access(DATACONN_PATH, X_OK) == 0;
}

static void stopDataconn(int cid) {
    for (int i = 0; i < MAX_PDP; i++) {
        if (cid > 0 && (i + 1) != cid) continue;
        if (s_sessions[i].pid > 0) {
            kill(s_sessions[i].pid, SIGTERM);
            waitpid(s_sessions[i].pid, NULL, 0);
        }
        s_sessions[i].pid = -1;
    }
}

static int dataconnAlive(int idx) {
    if (idx < 0 || idx >= MAX_PDP) return 0;
    if (s_sessions[idx].pid > 0 && waitpid(s_sessions[idx].pid, NULL, WNOHANG) == s_sessions[idx].pid) {
        RLOGE("kona-dataconn (cid %d) exited, data session lost", idx + 1);
        s_sessions[idx].pid = -1;
    }
    return s_sessions[idx].pid > 0;
}

static void sendDataconnCallList(RIL_Token *t);

static void onDataconnExit(void *param __unused) {
    sendDataconnCallList(NULL);
}

// The helper exits when the modem drops the session; tell Android so it sets up a new one.
static void *watchDataconn(void *arg) {
    FILE *out = arg;
    char line[256];
    while (fgets(line, sizeof(line), out)) {
        line[strcspn(line, "\r\n")] = '\0';
        RLOGD("kona-dataconn: %s", line);
    }
    fclose(out);
    static const struct timeval delay = {1, 0};
    RIL_requestTimedCallback(onDataconnExit, NULL, &delay);
    return NULL;
}

// Returns cid (1..MAX_PDP) once the helper reports "ready", or -1 on failure.
static int startDataconn(const char *apn, const char *pdp_type) {
    int fds[2];
    const char *family = "ipv4";
    if (pdp_type) {
        if (!strcmp(pdp_type, "IPV6")) {
            family = "ipv6";
        } else if (!strcmp(pdp_type, "IPV4V6")) {
            family = "ipv4v6";
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_PDP; i++) {
        if (!dataconnAlive(i)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        RLOGE("startDataconn: no free PDP slots (MAX_PDP=%d)", MAX_PDP);
        return -1;
    }

    int cid = slot + 1;
    if (pipe(fds) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        char mux_str[16];
        snprintf(mux_str, sizeof(mux_str), "%d", cid);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(DATACONN_PATH, DATACONN_PATH, apn ? apn : "", family, mux_str, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    struct DataconnSession *s = &s_sessions[slot];
    memset(s, 0, sizeof(*s));
    s->pid = pid;
    strlcpy(s->type, (pdp_type && *pdp_type) ? pdp_type : "IP", sizeof(s->type));

    FILE *out = fdopen(fds[0], "r");
    char line[256];
    int ready = 0;
    while (out && fgets(line, sizeof(line), out)) {
        line[strcspn(line, "\r\n")] = '\0';
        RLOGD("kona-dataconn[cid=%d]: %s", cid, line);
        if (!strcmp(line, "ready")) {
            ready = 1;
            break;
        } else if (!strncmp(line, "ifname=", 7)) {
            strlcpy(s->ifname, line + 7, sizeof(s->ifname));
        } else if (!strncmp(line, "address=", 8)) {
            if (s->addr[0]) strlcat(s->addr, " ", sizeof(s->addr));
            strlcat(s->addr, line + 8, sizeof(s->addr));
        } else if (!strncmp(line, "ipv6_address=", 13)) {
            if (s->addr[0]) strlcat(s->addr, " ", sizeof(s->addr));
            strlcat(s->addr, line + 13, sizeof(s->addr));
        } else if (!strncmp(line, "gateway=", 8)) {
            if (s->gw[0]) strlcat(s->gw, " ", sizeof(s->gw));
            strlcat(s->gw, line + 8, sizeof(s->gw));
        } else if (!strncmp(line, "dns=", 4)) {
            if (s->dns[0]) strlcat(s->dns, " ", sizeof(s->dns));
            strlcat(s->dns, line + 4, sizeof(s->dns));
        } else if (!strncmp(line, "pcscf=", 6)) {
            if (s->pcscf[0]) strlcat(s->pcscf, " ", sizeof(s->pcscf));
            strlcat(s->pcscf, line + 6, sizeof(s->pcscf));
        } else if (!strncmp(line, "protocol=", 9)) {
            strlcpy(s->type, line + 9, sizeof(s->type));
        } else if (!strncmp(line, "mtu=", 4)) {
            s->mtu = atoi(line + 4);
        }
    }
    if (!ready) {
        if (out) fclose(out); else close(fds[0]);
        stopDataconn(cid);
        return -1;
    }

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, watchDataconn, out) != 0) fclose(out);
    pthread_attr_destroy(&attr);
    return cid;
}

static void fillDataconnResponse(int cid, RIL_Data_Call_Response_v11 *r) {
    struct DataconnSession *s = &s_sessions[cid - 1];
    r->status = 0;
    r->suggestedRetryTime = -1;
    r->cid = cid;
    r->active = 2;  // link up
    r->type = s->type;
    r->ifname = s->ifname;
    r->addresses = s->addr;
    r->dnses = s->dns;
    r->gateways = s->gw;
    r->pcscf = s->pcscf;
    r->mtu = s->mtu;
}

static void sendDataconnCallList(RIL_Token *t) {
    RIL_Data_Call_Response_v11 responses[MAX_PDP];
    int n = 0;
    for (int i = 0; i < MAX_PDP; i++) {
        if (dataconnAlive(i)) {
            fillDataconnResponse(i + 1, &responses[n++]);
        }
    }
    if (t != NULL) {
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, n ? responses : NULL,
                              n * sizeof(RIL_Data_Call_Response_v11));
    } else {
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, n ? responses : NULL,
                                  n * sizeof(RIL_Data_Call_Response_v11));
    }
}

static void requestOrSendDataCallList(int cid, RIL_Token *t)
{
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;
    int err = -1;
    int n = 0;
    char *out = NULL;
    char propValue[PROP_VALUE_MAX] = {0};
    const char* radioInterfaceName = getRadioInterfaceName();

    if (useDataconn()) {
        sendDataconnCallList(t);
        return;
    }

    err = at_send_command_multiline ("AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response_v11 *responses = (n == 0) ? NULL :
                   alloca(n * sizeof(RIL_Data_Call_Response_v11));

    int i;
    for (i = 0; i < n; i++) {
        responses[i].status = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].ifname = "";
        responses[i].addresses = "";
        responses[i].dnses = "";
        responses[i].gateways = "";
        responses[i].pcscf = "";
        responses[i].mtu = 0;
    }

    RIL_Data_Call_Response_v11 *response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response->active);
        if (err < 0)
            goto error;

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline ("AT+CGDCONT?", "+CGDCONT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                      NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int ncid;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &ncid);
        if (err < 0)
            goto error;

        if (cid != ncid)
            continue;

        i = ncid - 1;

        if (i >= n || i < 0)
            goto error;

        // Assume no error
        responses[i].status = 0;

        // type
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int type_size = strlen(out) + 1;
        responses[i].type = alloca(type_size);
        strlcpy(responses[i].type, out, type_size);

        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int ifname_size = strlen(radioInterfaceName) + 1;
        responses[i].ifname = alloca(ifname_size);
        strlcpy(responses[i].ifname, radioInterfaceName, ifname_size);

        err = at_tok_nextstr(&line, &out);
        if (err < 0)
            goto error;

        int addresses_size = strlen(out) + 1;
        responses[i].addresses = alloca(addresses_size);
        strlcpy(responses[i].addresses, out, addresses_size);
#ifdef CUTTLEFISH_ENABLE
        set_Ip_Addr(responses[i].addresses, radioInterfaceName);
#endif

        if (isInEmulator()) {
            /* We are in the emulator - the dns servers are listed
                * by the following system properties, setup in
                * /system/etc/init.goldfish.sh:
                *  - vendor.net.eth0.dns1
                *  - vendor.net.eth0.dns2
                *  - vendor.net.eth0.dns3
                *  - vendor.net.eth0.dns4
                */
            const int   dnslist_sz = 128;
            char*       dnslist = alloca(dnslist_sz);
            const char* separator = "";
            int         nn;

            dnslist[0] = 0;
            for (nn = 1; nn <= 4; nn++) {
                /* Probe vendor.net.eth0.dns<n> */
                char  propName[PROP_NAME_MAX];
                char  propValue[PROP_VALUE_MAX];

                snprintf(propName, sizeof propName, "vendor.net.eth0.dns%d", nn);

                /* Ignore if undefined */
                if (property_get(propName, propValue, "") <= 0) {
                    continue;
                }

                /* Append the DNS IP address */
                strlcat(dnslist, separator, dnslist_sz);
                strlcat(dnslist, propValue, dnslist_sz);
                separator = " ";
            }
            responses[i].dnses = dnslist;

            if (property_get("vendor.net.eth0.gw", propValue, "") > 0) {
                responses[i].gateways = propValue;
            } else {
                responses[i].gateways = "";
            }
            responses[i].mtu = DEFAULT_MTU;
        } else {
            /* I don't know where we are, so use the public Google DNS
                * servers by default and no gateway.
                */
            responses[i].dnses = "8.8.8.8 8.8.4.4";
            responses[i].gateways = "";
        }
    }

    // If cid = -1, return the data call list without processing CGCONTRDP (setupDataCall)
    if (cid == -1) {
        if (t != NULL)
            RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                                  sizeof(RIL_Data_Call_Response_v11));
        else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, responses,
                                      n * sizeof(RIL_Data_Call_Response_v11));
        at_response_free(p_response);
        p_response = NULL;
        return;
    }

    at_response_free(p_response);
    p_response = NULL;

    char cmd[64] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CGCONTRDP=%d", cid);
    err = at_send_command_singleline(cmd, "+CGCONTRDP:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    int skip = 0;
    char *sskip = NULL;
    char *input = p_response->p_intermediates->line;

    int ncid = -1;
    err = at_tok_start(&input);
    if (err < 0) goto error;

    err = at_tok_nextint(&input, &ncid);  // cid
    if (err < 0) goto error;

    if (cid != ncid) goto error;

    i = ncid - 1;

    err = at_tok_nextint(&input, &skip);  // bearer_id
    if (err < 0) goto error;

    err = at_tok_nextstr(&input, &sskip);  // apn
    if (err < 0) goto error;

    err = at_tok_nextstr(&input, &sskip);  // local_addr_and_subnet_mask
    if (err < 0) goto error;

    err = at_tok_nextstr(
        &input, (responses) ? &responses[i].gateways : &sskip);  // gw_addr
    if (err < 0) goto error;

    err = at_tok_nextstr(
        &input, (responses) ? &responses[i].dnses : &sskip);  // dns_prim_addr
    if (err < 0) goto error;

    size_t response_size = 0;
    RIL_Data_Call_Response_v11 *presponse = NULL;
    if (responses) {
        if (i >= n || i < 0)
            goto error;
        presponse = &responses[i];
        response_size = sizeof(*presponse);
    }

    if (t != NULL)
      RIL_onRequestComplete(*t, RIL_E_SUCCESS,
                            presponse, response_size);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  responses, n * response_size);

    at_response_free(p_response);
    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                                  NULL, 0);
    at_response_free(p_response);
}

static void requestQueryNetworkSelectionMode(
                void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    RLOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void sendCallStateChanged(void *param __unused)
{
    RIL_onUnsolicitedResponse (
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        NULL, 0);
}

#define VOICECALL_PATH "/vendor/bin/kona-voicecall"

static pid_t s_voicecall_pid = -1;

// The modem only exchanges call audio with the DSP while the voice graph runs.
static void setVoicePath(int on) {
    if (s_voicecall_pid > 0 && waitpid(s_voicecall_pid, NULL, WNOHANG) == s_voicecall_pid) {
        RLOGE("kona-voicecall exited");
        s_voicecall_pid = -1;
    }
    if (on && s_voicecall_pid < 0 && access(VOICECALL_PATH, X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(VOICECALL_PATH, VOICECALL_PATH, (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            RLOGD("kona-voicecall started");
            s_voicecall_pid = pid;
        }
    } else if (!on && s_voicecall_pid > 0) {
        kill(s_voicecall_pid, SIGTERM);
        waitpid(s_voicecall_pid, NULL, 0);
        RLOGD("kona-voicecall stopped");
        s_voicecall_pid = -1;
    }
}

static void requestGetCurrentCalls(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response;
    ATLine *p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call *p_calls;
    RIL_Call **pp_calls;
    int i;
    int needRepoll = 0;
    int needVoicePath = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline ("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call **)alloca(countCalls * sizeof(RIL_Call *));
    p_calls = (RIL_Call *)alloca(countCalls * sizeof(RIL_Call));
    memset (p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for(i = 0; i < countCalls ; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next
    ) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        // The modem lists its packet data session as a mode 1 call.
        if (err != 0 || !p_calls[countValidCalls].isVoice) {
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING
        ) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING
        ) {
            needRepoll = 1;
        }

        // The voice graphs must be up before the call connects: the modem configures
        // them right when it enables its voice session.
        needVoicePath = 1;

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
            && s_incomingOrWaitingLine < 0
            && s_expectAnswer == 0
    ) {
        for (i = 0; i < countValidCalls ; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                    && p_calls[i].state == RIL_CALL_ACTIVE
                    && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX
            ) {
                RLOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n", s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    setVoicePath(needVoicePath);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
            countValidCalls * sizeof (RIL_Call *));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) {  // We don't seem to get a "NO CARRIER" message from
                            // smd, so we're forced to poll until the call ends.
#else
    // This modem sends no "NO CARRIER" on our port either, so keep polling during calls.
    if (needRepoll || countValidCalls) {
#endif
        RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
#endif
}

static void requestDial(void *data, size_t datalen __unused, RIL_Token t)
{
    RIL_Dial *p_dial;
    char *cmd;
    const char *clir;

    p_dial = (RIL_Dial *)data;

    switch (p_dial->clir) {
        case 1: clir = "I"; break;  /*invocation*/
        case 2: clir = "i"; break;  /*suppression*/
        default:
        case 0: clir = ""; break;   /*subscription default*/
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);

    at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestWriteSmsToSim(void *data, size_t datalen __unused, RIL_Token t)
{
    RIL_SMS_WriteArgs *p_args;
    char *cmd;
    int length;
    int err;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu)/2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestHangup(void *data, size_t datalen __unused, RIL_Token t)
{
    int *p_line;

    char *cmd;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }
    p_line = (int *)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);

    at_send_command(cmd, NULL);

    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void initEmptySignalStrength(RIL_SignalStrength_v12 *ss)
{
    memset(ss, 0, sizeof(*ss));
    ss->GW_SignalStrength.signalStrength = 99;
    ss->GW_SignalStrength.bitErrorRate = 99;
    ss->CDMA_SignalStrength.dbm = -1;
    ss->CDMA_SignalStrength.ecio = -1;
    ss->EVDO_SignalStrength.dbm = -1;
    ss->EVDO_SignalStrength.ecio = -1;
    ss->EVDO_SignalStrength.signalNoiseRatio = -1;
    ss->LTE_SignalStrength.signalStrength = 99;
    ss->LTE_SignalStrength.rsrp = INT_MAX;
    ss->LTE_SignalStrength.rsrq = INT_MAX;
    ss->LTE_SignalStrength.rssnr = INT_MAX;
    ss->LTE_SignalStrength.cqi = INT_MAX;
    ss->LTE_SignalStrength.timingAdvance = INT_MAX;
    ss->TD_SCDMA_SignalStrength.rscp = INT_MAX;
    ss->WCDMA_SignalStrength.signalStrength = 99;
    ss->WCDMA_SignalStrength.bitErrorRate = 99;
    ss->NR_SignalStrength.ssRsrp = INT_MAX;
    ss->NR_SignalStrength.ssRsrq = INT_MAX;
    ss->NR_SignalStrength.ssSinr = INT_MAX;
    ss->NR_SignalStrength.csiRsrp = INT_MAX;
    ss->NR_SignalStrength.csiRsrq = INT_MAX;
    ss->NR_SignalStrength.csiSinr = INT_MAX;
}

static uint32_t s_qmiNasNode = 0;
static uint32_t s_qmiNasPort = 0;
static int s_qmiNasResolved = 0;
static uint32_t s_qmiUimNode = 0;
static uint32_t s_qmiUimPort = 0;
static int s_qmiUimResolved = 0;
static uint32_t s_qmiDmsNode = 0;
static uint32_t s_qmiDmsPort = 0;
static int s_qmiDmsResolved = 0;
static uint16_t s_qmiTxn = 1;

static int qmiLookupService(uint32_t service, uint32_t *out_node, uint32_t *out_port)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_qrtr local;
    socklen_t sl = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &sl) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_qrtr ctrl = {
        .sq_family = AF_QIPCRTR,
        .sq_node = local.sq_node,
        .sq_port = QRTR_PORT_CTRL,
    };
    struct qrtr_ctrl_pkt req;
    memset(&req, 0, sizeof(req));
    req.cmd = QRTR_TYPE_NEW_LOOKUP;
    req.server.service = service;
    req.server.instance = 0;

    if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) {
        close(fd);
        return -1;
    }

    int found = 0;
    for (;;) {
        struct qrtr_ctrl_pkt resp;
        ssize_t n = recv(fd, &resp, sizeof(resp), 0);
        if (n < (ssize_t)sizeof(resp)) break;
        if (resp.cmd == QRTR_TYPE_NEW_SERVER) {
            if (resp.server.service == 0 && resp.server.node == 0 && resp.server.port == 0)
                break;
            if (resp.server.service == service) {
                *out_node = resp.server.node;
                *out_port = resp.server.port;
                found = 1;
            }
        }
    }
    close(fd);
    return found ? 0 : -1;
}

static ssize_t qmiSendRecvFd(int fd, uint32_t node, uint32_t port, uint16_t msg_id,
                             const uint8_t *tlvs, size_t tlv_len,
                             uint8_t *resp, size_t resp_cap, int timeout_ms)
{
    if (fd < 0 || tlv_len > 1000) return -1;

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_qrtr dst = {
        .sq_family = AF_QIPCRTR,
        .sq_node = node,
        .sq_port = port,
    };

    uint16_t txn = s_qmiTxn++;
    if (s_qmiTxn == 0) s_qmiTxn = 1;

    uint8_t pkt[1024];
    pkt[0] = 0x00;
    pkt[1] = (uint8_t)(txn & 0xff);
    pkt[2] = (uint8_t)((txn >> 8) & 0xff);
    pkt[3] = (uint8_t)(msg_id & 0xff);
    pkt[4] = (uint8_t)((msg_id >> 8) & 0xff);
    pkt[5] = (uint8_t)(tlv_len & 0xff);
    pkt[6] = (uint8_t)((tlv_len >> 8) & 0xff);
    if (tlv_len > 0 && tlvs) {
        memcpy(pkt + 7, tlvs, tlv_len);
    }

    if (sendto(fd, pkt, 7 + tlv_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        return -1;
    }

    ssize_t n = recv(fd, resp, resp_cap, 0);
    if (n < 7) return -1;
    uint16_t rmsg = (uint16_t)(resp[3] | (resp[4] << 8));
    if (rmsg != msg_id) return -1;
    return n;
}

static ssize_t qmiSendRecv(uint32_t node, uint32_t port, uint16_t msg_id,
                           const uint8_t *tlvs, size_t tlv_len,
                           uint8_t *resp, size_t resp_cap, int timeout_ms)
{
    if (tlv_len > 1000) return -1;
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    ssize_t n = qmiSendRecvFd(fd, node, port, msg_id, tlvs, tlv_len,
                              resp, resp_cap, timeout_ms);
    close(fd);
    return n;
}

static int qmiCheckResultTlv(const uint8_t *buf, size_t len, uint16_t *err_out)
{
    size_t pos = 7;
    while (pos + 3 <= len) {
        uint8_t t = buf[pos];
        uint16_t l = (uint16_t)(buf[pos + 1] | (buf[pos + 2] << 8));
        pos += 3;
        if (pos + l > len) break;
        if (t == 0x02 && l >= 4) {
            uint16_t res = (uint16_t)(buf[pos] | (buf[pos + 1] << 8));
            uint16_t err = (uint16_t)(buf[pos + 2] | (buf[pos + 3] << 8));
            if (err_out) *err_out = err;
            return (res == 0) ? 0 : -1;
        }
        pos += l;
    }
    return -1;
}

static int qmiEnsureNas(void)
{
    if (!s_qmiNasResolved) {
        if (qmiLookupService(3 /* QMI_SERVICE_NAS */, &s_qmiNasNode, &s_qmiNasPort) < 0) {
            return -1;
        }
        s_qmiNasResolved = 1;
        RLOGI("QMI NAS resolved at QRTR node=%u port=%u", s_qmiNasNode, s_qmiNasPort);
    }
    return 0;
}

static int qmiEnsureUim(void)
{
    if (!s_qmiUimResolved) {
        if (qmiLookupService(11 /* QMI_SERVICE_UIM */, &s_qmiUimNode, &s_qmiUimPort) < 0) {
            return -1;
        }
        s_qmiUimResolved = 1;
        RLOGI("QMI UIM resolved at QRTR node=%u port=%u", s_qmiUimNode, s_qmiUimPort);
    }
    return 0;
}

static int qmiEnsureDms(void)
{
    if (!s_qmiDmsResolved) {
        if (qmiLookupService(2 /* QMI_SERVICE_DMS */, &s_qmiDmsNode, &s_qmiDmsPort) < 0) {
            return -1;
        }
        s_qmiDmsResolved = 1;
        RLOGI("QMI DMS resolved at QRTR node=%u port=%u", s_qmiDmsNode, s_qmiDmsPort);
    }
    return 0;
}

static int qmiExtractStringTlv(const uint8_t *buf, size_t len, uint8_t target_tlv,
                               char *out, size_t out_cap)
{
    size_t pos = 7;
    while (pos + 3 <= len) {
        uint8_t t = buf[pos];
        uint16_t l = (uint16_t)(buf[pos + 1] | (buf[pos + 2] << 8));
        pos += 3;
        if (pos + l > len) break;
        if (t == target_tlv && l > 0) {
            size_t copy = l < out_cap - 1 ? l : out_cap - 1;
            memcpy(out, buf + pos, copy);
            out[copy] = '\0';
            while (copy > 0 && (out[copy - 1] == '\0' || out[copy - 1] == ' ' ||
                                out[copy - 1] == '\r' || out[copy - 1] == '\n')) {
                out[--copy] = '\0';
            }
            return copy > 0 ? 0 : -1;
        }
        pos += l;
    }
    return -1;
}

static int qmiDmsGetBasebandVersion(char *out, size_t out_cap)
{
    if (!out || out_cap < 2) return -1;
    if (qmiEnsureDms() < 0) return -1;

    uint8_t resp[1024];
    ssize_t n = qmiSendRecv(s_qmiDmsNode, s_qmiDmsPort, 0x0051 /* QMI_DMS_GET_SW_VERSION */,
                            NULL, 0, resp, sizeof(resp), 2000);
    if (n > 7 && qmiCheckResultTlv(resp, (size_t)n, NULL) == 0) {
        if (qmiExtractStringTlv(resp, (size_t)n, 0x11, out, out_cap) == 0) {
            /*
             * Android GsmCdmaPhone truncates baseband versions > 45 chars
             * (PROP_VALUE_MAX / 2) from the front, clipping "MPSS.HI.2".
             * Trim any trailing ".<numeric-build>" suffix after "PACK-<n>"
             * so the full MPSS version fits cleanly within 45 chars.
             */
            if (strlen(out) > 45) {
                char *pack = strstr(out, "PACK-");
                if (pack) {
                    char *dot = strchr(pack, '.');
                    if (dot && (size_t)(dot - out) <= 45) {
                        *dot = '\0';
                    }
                }
                if (strlen(out) > 45) {
                    out[45] = '\0';
                }
            }
            return 0;
        }
        if (qmiExtractStringTlv(resp, (size_t)n, 0x01, out, out_cap) == 0) {
            return 0;
        }
    }

    n = qmiSendRecv(s_qmiDmsNode, s_qmiDmsPort, 0x0023 /* QMI_DMS_GET_DEVICE_REV_ID */,
                    NULL, 0, resp, sizeof(resp), 2000);
    if (n > 7 && qmiCheckResultTlv(resp, (size_t)n, NULL) == 0) {
        if (qmiExtractStringTlv(resp, (size_t)n, 0x01, out, out_cap) == 0) {
            return 0;
        }
    }

    s_qmiDmsResolved = 0;
    return -1;
}

static int qmiDmsGetSerialNumbersForSlot(int slot_id,
                                         char *imei, size_t imei_cap,
                                         char *imeisv, size_t imeisv_cap,
                                         char *esn, size_t esn_cap,
                                         char *meid, size_t meid_cap)
{
    if (imei && imei_cap) imei[0] = '\0';
    if (imeisv && imeisv_cap) imeisv[0] = '\0';
    if (esn && esn_cap) esn[0] = '\0';
    if (meid && meid_cap) meid[0] = '\0';

    if (qmiEnsureDms() < 0) return -1;

    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    /*
     * Bind this QRTR client port to the requested subscription before reading
     * serial numbers so SDX55 returns slot 1 (primary=1) or slot 2 (secondary=2)
     * IMEI / IMEI SV / MEID cleanly.
     */
    uint32_t subs = (slot_id == 1) ? 2u /* DMS_SECONDARY_SUBS_V01 */
                                   : 1u /* DMS_PRIMARY_SUBS_V01 */;
    uint8_t bind_tlv[] = {
        0x01, 0x04, 0x00,
        (uint8_t)(subs & 0xff), (uint8_t)((subs >> 8) & 0xff),
        (uint8_t)((subs >> 16) & 0xff), (uint8_t)((subs >> 24) & 0xff),
    };
    uint8_t resp[512];
    ssize_t bn = qmiSendRecvFd(fd, s_qmiDmsNode, s_qmiDmsPort,
                               0x0054 /* QMI_DMS_BIND_SUBSCRIPTION */,
                               bind_tlv, sizeof(bind_tlv), resp, sizeof(resp), 2000);
    if (bn <= 7 || qmiCheckResultTlv(resp, (size_t)bn, NULL) < 0) {
        RLOGW("QMI DMS BIND_SUBSCRIPTION(slot=%d, subs=%u) failed (bn=%zd)", slot_id, subs, bn);
    }

    ssize_t n = qmiSendRecvFd(fd, s_qmiDmsNode, s_qmiDmsPort,
                              0x0025 /* QMI_DMS_GET_DEVICE_SERIAL_NUMBERS */,
                              NULL, 0, resp, sizeof(resp), 2000);
    close(fd);

    if (n <= 7 || qmiCheckResultTlv(resp, (size_t)n, NULL) < 0) {
        s_qmiDmsResolved = 0;
        return -1;
    }

    if (imei && imei_cap) qmiExtractStringTlv(resp, (size_t)n, 0x11, imei, imei_cap);
    if (imeisv && imeisv_cap) qmiExtractStringTlv(resp, (size_t)n, 0x13, imeisv, imeisv_cap);
    if (esn && esn_cap) {
        qmiExtractStringTlv(resp, (size_t)n, 0x10, esn, esn_cap);
        if (strcmp(esn, "0") == 0) esn[0] = '\0';
    }
    if (meid && meid_cap) qmiExtractStringTlv(resp, (size_t)n, 0x12, meid, meid_cap);
    return (imei && imei[0] != '\0') ? 0 : -1;
}

static int qmiDmsGetSerialNumbers(char *imei, size_t imei_cap,
                                  char *imeisv, size_t imeisv_cap,
                                  char *esn, size_t esn_cap,
                                  char *meid, size_t meid_cap)
{
    return qmiDmsGetSerialNumbersForSlot(0, imei, imei_cap, imeisv, imeisv_cap,
                                         esn, esn_cap, meid, meid_cap);
}

typedef struct {
    int card_present;   /* 1 = PRESENT, 0 = ABSENT */
    int slot_active;    /* 1 = ACTIVE, 0 = INACTIVE */
    int logical_slot;   /* 0-based logical slot id */
    char iccid[32];     /* ASCII ICCID digits */
} QmiUimSlotInfo;

static int qmiUimGetSlotsStatus(QmiUimSlotInfo *slots, int max_slots)
{
    if (!slots || max_slots <= 0) return -1;
    if (qmiEnsureUim() < 0) return -1;

    uint8_t resp[512];
    ssize_t n = qmiSendRecv(s_qmiUimNode, s_qmiUimPort,
                            0x0047 /* QMI_UIM_GET_SLOTS_STATUS */,
                            NULL, 0, resp, sizeof(resp), 2000);
    if (n <= 7 || qmiCheckResultTlv(resp, (size_t)n, NULL) < 0) {
        if (n < 0) s_qmiUimResolved = 0;
        return -1;
    }

    size_t pos = 7;
    while (pos + 3 <= (size_t)n) {
        uint8_t t = resp[pos];
        uint16_t l = (uint16_t)(resp[pos + 1] | (resp[pos + 2] << 8));
        pos += 3;
        if (pos + l > (size_t)n) break;
        if (t == 0x10 && l >= 1) {
            size_t end = pos + l;
            uint8_t num = resp[pos++];
            int parsed = 0;
            for (uint8_t i = 0; i < num && pos + 10 <= end; i++) {
                uint32_t card_state = (uint32_t)resp[pos] |
                                      ((uint32_t)resp[pos + 1] << 8) |
                                      ((uint32_t)resp[pos + 2] << 16) |
                                      ((uint32_t)resp[pos + 3] << 24);
                pos += 4;
                uint32_t slot_state = (uint32_t)resp[pos] |
                                      ((uint32_t)resp[pos + 1] << 8) |
                                      ((uint32_t)resp[pos + 2] << 16) |
                                      ((uint32_t)resp[pos + 3] << 24);
                pos += 4;
                uint8_t logical_slot = resp[pos++];
                uint8_t iccid_len = resp[pos++];
                if (pos + iccid_len > end) break;

                if (parsed < max_slots) {
                    QmiUimSlotInfo *dst = &slots[parsed];
                    memset(dst, 0, sizeof(*dst));
                    dst->card_present = (card_state == 2u) ? 1 : 0;
                    dst->slot_active = (slot_state == 1u) ? 1 : 0;
                    dst->logical_slot = (logical_slot > 0) ? (int)(logical_slot - 1) : parsed;
                    size_t out_i = 0;
                    for (uint8_t b = 0; b < iccid_len && out_i + 2 < sizeof(dst->iccid); b++) {
                        uint8_t lo = resp[pos + b] & 0x0f;
                        uint8_t hi = (resp[pos + b] >> 4) & 0x0f;
                        if (lo <= 9) dst->iccid[out_i++] = (char)('0' + lo);
                        else break;
                        if (hi <= 9) dst->iccid[out_i++] = (char)('0' + hi);
                        else break;
                    }
                    dst->iccid[out_i] = '\0';
                    parsed++;
                }
                pos += iccid_len;
            }
            return parsed;
        }
        pos += l;
    }
    return -1;
}

static uint16_t s_lastModePref = 0;
static int s_nrNudgeActive = 0;
static uint16_t s_pendingModePref = 0;
static uint8_t s_pendingAcq[8];
static uint8_t s_pendingAcqLen = 0;
static uintptr_t s_pendingModeGen = 0;

static int qmiNasSendSysSelPref(uint16_t mode_pref, const uint8_t *acq, uint8_t acq_len,
                                uint8_t duration)
{
    uint8_t tlvs[64];
    size_t off = 0;
    /* TLV 0x11: Mode Preference */
    tlvs[off++] = 0x11; tlvs[off++] = 0x02; tlvs[off++] = 0x00;
    tlvs[off++] = (uint8_t)(mode_pref & 0xff);
    tlvs[off++] = (uint8_t)((mode_pref >> 8) & 0xff);
    /* TLV 0x16: Network Selection Preference = AUTOMATIC (0) */
    tlvs[off++] = 0x16; tlvs[off++] = 0x05; tlvs[off++] = 0x00;
    tlvs[off++] = 0x00; tlvs[off++] = 0x00; tlvs[off++] = 0x00;
    tlvs[off++] = 0x00; tlvs[off++] = 0x00;
    /* TLV 0x17: Change Duration (0 = POWER_CYCLE, 1 = PERMANENT) */
    tlvs[off++] = 0x17; tlvs[off++] = 0x01; tlvs[off++] = 0x00;
    tlvs[off++] = duration;
    /* TLV 0x1E: Acquisition Order Preference (highest RAT first: NR5G -> LTE -> UMTS -> GSM) */
    if (acq && acq_len > 0) {
        uint16_t tlen = (uint16_t)(1 + acq_len);
        tlvs[off++] = 0x1E;
        tlvs[off++] = (uint8_t)(tlen & 0xff);
        tlvs[off++] = (uint8_t)((tlen >> 8) & 0xff);
        tlvs[off++] = acq_len;
        memcpy(tlvs + off, acq, acq_len);
        off += acq_len;
    }

    uint8_t resp[256];
    ssize_t n = qmiSendRecv(s_qmiNasNode, s_qmiNasPort, 0x0033 /* SetSystemSelectionPreference */,
                            tlvs, off, resp, sizeof(resp), 2000);
    uint16_t qerr = 0;
    if (n < 0 || qmiCheckResultTlv(resp, (size_t)n, &qerr) < 0) {
        /* Fallback to minimal Mode Preference + Change Duration TLVs if extended TLVs were rejected */
        uint8_t min_tlvs[] = {
            0x11, 0x02, 0x00, (uint8_t)(mode_pref & 0xff), (uint8_t)((mode_pref >> 8) & 0xff),
            0x17, 0x01, 0x00, duration,
        };
        n = qmiSendRecv(s_qmiNasNode, s_qmiNasPort, 0x0033,
                        min_tlvs, sizeof(min_tlvs), resp, sizeof(resp), 2000);
        if (n < 0) {
            s_qmiNasResolved = 0;
            return -1;
        }
        if (qmiCheckResultTlv(resp, (size_t)n, &qerr) < 0) {
            RLOGW("QMI NAS SetSystemSelectionPreference(0x%04x) QMI err=0x%04x", mode_pref, qerr);
            return -1;
        }
    }
    return 0;
}

static void restorePendingModePrefCallback(void *param)
{
    uintptr_t gen = (uintptr_t)param;
    if (!s_nrNudgeActive || gen != s_pendingModeGen) {
        return;
    }
    s_nrNudgeActive = 0;

    if (sState != RADIO_STATE_ON || qmiEnsureNas() < 0) {
        return;
    }

    if (qmiNasSendSysSelPref(s_pendingModePref, s_pendingAcq, s_pendingAcqLen, 0x01) == 0) {
        s_lastModePref = s_pendingModePref;
        RLOGI("QMI NAS NR-nudge complete -> restored permanent mode_pref=0x%04x",
              s_pendingModePref);
    }
    notifyNetworkAndSignal(NULL);
}

static int qmiNasSetModePreference(int raf_bitmap)
{
    if (qmiEnsureNas() < 0) return -1;

    uint16_t mode_pref = 0;
    uint8_t acq[8];
    uint8_t acq_len = 0;
    if (raf_bitmap & NR) {
        mode_pref |= (1u << 6);
        acq[acq_len++] = 0x0C; /* QMI_NAS_RADIO_INTERFACE_5GNR */
    }
    if (raf_bitmap & (LTE | RAF_LTE_CA)) {
        mode_pref |= (1u << 4);
        acq[acq_len++] = 0x08; /* QMI_NAS_RADIO_INTERFACE_LTE */
    }
    if (raf_bitmap & WCDMA) {
        mode_pref |= (1u << 3);
        acq[acq_len++] = 0x05; /* QMI_NAS_RADIO_INTERFACE_UMTS */
    }
    if (raf_bitmap & RAF_TD_SCDMA) {
        mode_pref |= (1u << 5);
        acq[acq_len++] = 0x09; /* QMI_NAS_RADIO_INTERFACE_TD_SCDMA */
    }
    if (raf_bitmap & GSM) {
        mode_pref |= (1u << 2);
        acq[acq_len++] = 0x04; /* QMI_NAS_RADIO_INTERFACE_GSM */
    }
    if (raf_bitmap & EVDO) {
        mode_pref |= (1u << 1);
        acq[acq_len++] = 0x02; /* QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO */
    }
    if (raf_bitmap & CDMA) {
        mode_pref |= (1u << 0);
        acq[acq_len++] = 0x01; /* QMI_NAS_RADIO_INTERFACE_CDMA_1X */
    }

    if (mode_pref == 0) {
        RLOGW("qmiNasSetModePreference: empty mode_pref for bitmap 0x%x, ignoring", raf_bitmap);
        return -1;
    }

    /*
     * Telephony frequently sends duplicate SET_ALLOWED_NETWORK_TYPES_BITMAP
     * requests ~30-50ms apart; don't abort an in-flight NR-only nudge for the
     * exact same target mode_pref.
     */
    if (s_nrNudgeActive && mode_pref == s_pendingModePref) {
        RLOGI("QMI NAS SetSystemSelectionPreference bitmap=0x%x -> NR-nudge already in flight for 0x%04x",
              raf_bitmap, mode_pref);
        return 0;
    }

    uint16_t prev_mode = s_lastModePref;
    s_nrNudgeActive = 0;
    s_pendingModeGen++;

    /*
     * When upgrading from LTE/non-NR to 5G+LTE, SDX55 stays camped on LTE
     * (and treats AT+COPS=0 as a no-op when already registered automatically).
     * Briefly apply a temporary (POWER_CYCLE) NR-only mode_pref (0x0040) so
     * the modem immediately hands over to 5G NR SA (~1s, keeping IMS up),
     * then restore the full permanent NR+LTE preference after 2.5s so normal
     * 4G fallback remains available if NR signal is absent or lost.
     */
    if ((mode_pref & (1u << 6)) && (mode_pref != (1u << 6)) &&
            prev_mode != 0 && !(prev_mode & (1u << 6)) &&
            sState == RADIO_STATE_ON) {
        static const uint8_t nr_only_acq[] = { 0x0C };
        if (qmiNasSendSysSelPref((uint16_t)(1u << 6), nr_only_acq, 1, 0x00) == 0) {
            s_lastModePref = mode_pref;
            s_pendingModePref = mode_pref;
            memcpy(s_pendingAcq, acq, acq_len);
            s_pendingAcqLen = acq_len;
            s_nrNudgeActive = 1;
            uintptr_t gen = s_pendingModeGen;
            static const struct timeval nudge_delay = {2, 500000};
            RIL_requestTimedCallback(restorePendingModePrefCallback, (void *)gen, &nudge_delay);
            RLOGI("QMI NAS SetSystemSelectionPreference bitmap=0x%x -> NR-only nudge (0x0040), restoring 0x%04x in 2.5s",
                  raf_bitmap, mode_pref);
            return 0;
        }
        /* If temporary NR-only nudge failed, fall through to normal permanent set. */
    }

    if (qmiNasSendSysSelPref(mode_pref, acq, acq_len, 0x01) < 0) {
        return -1;
    }

    s_lastModePref = mode_pref;
    RLOGI("QMI NAS SetSystemSelectionPreference bitmap=0x%x -> mode_pref=0x%04x OK",
          raf_bitmap, mode_pref);
    return 0;
}

static int qmiUimOpenLogicalChannel(const uint8_t *aid, size_t aid_len, int p2,
                                    uint8_t *out_ch, uint8_t *out_sel, size_t *out_sel_len,
                                    uint8_t *out_sw1, uint8_t *out_sw2)
{
    if (qmiEnsureUim() < 0) return -1;

    uint8_t tlvs[128];
    size_t off = 0;
    /* TLV 0x01: Slot = 1 */
    tlvs[off++] = 0x01; tlvs[off++] = 0x01; tlvs[off++] = 0x00; tlvs[off++] = 0x01;
    /* TLV 0x10: AID (optional) */
    if (aid && aid_len > 0 && aid_len <= 64) {
        tlvs[off++] = 0x10;
        tlvs[off++] = (uint8_t)(aid_len + 1);
        tlvs[off++] = 0x00;
        tlvs[off++] = (uint8_t)aid_len;
        memcpy(tlvs + off, aid, aid_len);
        off += aid_len;
    }
    /* TLV 0x11: File Control Information (optional, mapped from ISO 7816-4 P2) */
    if (p2 >= 0) {
        uint8_t fci = 0; /* FCP */
        if ((p2 & 0x0c) == 0x00) fci = 2;      /* FCI */
        else if ((p2 & 0x0c) == 0x04) fci = 0; /* FCP */
        else if ((p2 & 0x0c) == 0x08) fci = 1; /* FMD */
        else if ((p2 & 0x0c) == 0x0c) fci = 3; /* NO DATA */
        tlvs[off++] = 0x11; tlvs[off++] = 0x01; tlvs[off++] = 0x00; tlvs[off++] = fci;
    }

    uint8_t resp[512];
    ssize_t n = qmiSendRecv(s_qmiUimNode, s_qmiUimPort, 0x0042 /* OpenLogicalChannel */,
                            tlvs, off, resp, sizeof(resp), 2000);
    if (n < 0) {
        s_qmiUimResolved = 0;
        return -1;
    }
    uint16_t qerr = 0;
    if (qmiCheckResultTlv(resp, (size_t)n, &qerr) < 0) {
        RLOGW("QMI UIM OpenLogicalChannel failed err=0x%04x", qerr);
        return -1;
    }

    int got_ch = 0;
    if (out_sel_len) *out_sel_len = 0;
    if (out_sw1) *out_sw1 = 0x90;
    if (out_sw2) *out_sw2 = 0x00;

    size_t pos = 7;
    while (pos + 3 <= (size_t)n) {
        uint8_t t = resp[pos];
        uint16_t l = (uint16_t)(resp[pos + 1] | (resp[pos + 2] << 8));
        pos += 3;
        if (pos + l > (size_t)n) break;
        if (t == 0x10 && l >= 1) {
            if (out_ch) *out_ch = resp[pos];
            got_ch = 1;
        } else if (t == 0x11 && l >= 2) {
            if (out_sw1) *out_sw1 = resp[pos];
            if (out_sw2) *out_sw2 = resp[pos + 1];
        } else if (t == 0x12 && l >= 1 && out_sel && out_sel_len) {
            size_t slen = resp[pos];
            if (slen > (size_t)(l - 1)) slen = (size_t)(l - 1);
            if (slen > 255) slen = 255;
            memcpy(out_sel, resp + pos + 1, slen);
            *out_sel_len = slen;
        }
        pos += l;
    }
    return got_ch ? 0 : -1;
}

static int qmiUimCloseLogicalChannel(uint8_t ch)
{
    if (qmiEnsureUim() < 0) return -1;
    uint8_t tlvs[] = {
        0x01, 0x01, 0x00, 0x01, /* Slot = 1 */
        0x11, 0x01, 0x00, ch,   /* Channel ID to close */
    };
    uint8_t resp[256];
    ssize_t n = qmiSendRecv(s_qmiUimNode, s_qmiUimPort, 0x003F /* LogicalChannel */,
                            tlvs, sizeof(tlvs), resp, sizeof(resp), 2000);
    if (n < 0) {
        s_qmiUimResolved = 0;
        return -1;
    }
    uint16_t qerr = 0;
    return qmiCheckResultTlv(resp, (size_t)n, &qerr);
}

static int qmiUimSendApdu(uint8_t ch, const uint8_t *apdu, size_t apdu_len,
                          uint8_t *out_resp, size_t resp_cap, size_t *out_resp_len)
{
    if (qmiEnsureUim() < 0 || !apdu || apdu_len == 0 || apdu_len > 512) return -1;

    uint8_t tlvs[600];
    size_t off = 0;
    /* TLV 0x01: Slot = 1 */
    tlvs[off++] = 0x01; tlvs[off++] = 0x01; tlvs[off++] = 0x00; tlvs[off++] = 0x01;
    /* TLV 0x02: APDU (uint16 len + bytes) */
    uint16_t tlv2_len = (uint16_t)(apdu_len + 2);
    tlvs[off++] = 0x02;
    tlvs[off++] = (uint8_t)(tlv2_len & 0xff);
    tlvs[off++] = (uint8_t)((tlv2_len >> 8) & 0xff);
    tlvs[off++] = (uint8_t)(apdu_len & 0xff);
    tlvs[off++] = (uint8_t)((apdu_len >> 8) & 0xff);
    memcpy(tlvs + off, apdu, apdu_len);
    off += apdu_len;
    /* TLV 0x10: Channel ID */
    tlvs[off++] = 0x10; tlvs[off++] = 0x01; tlvs[off++] = 0x00; tlvs[off++] = ch;

    uint8_t resp[1024];
    ssize_t n = qmiSendRecv(s_qmiUimNode, s_qmiUimPort, 0x003B /* SendAPDU */,
                            tlvs, off, resp, sizeof(resp), 3000);
    if (n < 0) {
        s_qmiUimResolved = 0;
        return -1;
    }
    uint16_t qerr = 0;
    if (qmiCheckResultTlv(resp, (size_t)n, &qerr) < 0) {
        RLOGW("QMI UIM SendAPDU(ch=%u) failed err=0x%04x", ch, qerr);
        return -1;
    }

    size_t pos = 7;
    while (pos + 3 <= (size_t)n) {
        uint8_t t = resp[pos];
        uint16_t l = (uint16_t)(resp[pos + 1] | (resp[pos + 2] << 8));
        pos += 3;
        if (pos + l > (size_t)n) break;
        if (t == 0x10 && l >= 2) {
            uint16_t rlen = (uint16_t)(resp[pos] | (resp[pos + 1] << 8));
            if (rlen > l - 2) rlen = (uint16_t)(l - 2);
            if (rlen > resp_cap) rlen = (uint16_t)resp_cap;
            if (out_resp && rlen > 0) memcpy(out_resp, resp + pos + 2, rlen);
            if (out_resp_len) *out_resp_len = rlen;
            return 0;
        }
        pos += l;
    }
    return -1;
}

static int readQmiNasSignalStrength(RIL_SignalStrength_v12 *ss)
{
    if (qmiEnsureNas() < 0) return -1;

    uint8_t buf[512];
    ssize_t n = qmiSendRecv(s_qmiNasNode, s_qmiNasPort, 0x004f /* GetSignalInfo */,
                            NULL, 0, buf, sizeof(buf), 250);
    if (n < 7) {
        s_qmiNasResolved = 0;
        return -1;
    }

    initEmptySignalStrength(ss);
    int got_any = 0;
    int got_lte = 0;
    int got_nr = 0;
    size_t pos = 7;
    while (pos + 3 <= (size_t)n) {
        uint8_t t = buf[pos];
        uint16_t l = (uint16_t)(buf[pos + 1] | (buf[pos + 2] << 8));
        pos += 3;
        if (pos + l > (size_t)n) break;

        if (t == 0x12 && l >= 1) {
            /* GSM RSSI (dBm) */
            int8_t rssi = (int8_t)buf[pos];
            if (rssi <= -51 && rssi >= -113) {
                ss->GW_SignalStrength.signalStrength = (rssi + 113) / 2;
                got_any = 1;
            }
        } else if (t == 0x13 && l >= 3) {
            /* WCDMA RSSI (dBm) */
            int8_t rssi = (int8_t)buf[pos];
            if (rssi <= -51 && rssi >= -113) {
                ss->WCDMA_SignalStrength.signalStrength = (rssi + 113) / 2;
                got_any = 1;
            }
        } else if (t == 0x14 && l >= 6) {
            /* LTE: int8 rssi, int8 rsrq, int16 rsrp, int16 snr (0.1 dB) */
            int8_t rssi = (int8_t)buf[pos];
            int8_t rsrq = (int8_t)buf[pos + 1];
            int16_t rsrp = (int16_t)(buf[pos + 2] | (buf[pos + 3] << 8));
            int16_t snr = (int16_t)(buf[pos + 4] | (buf[pos + 5] << 8));
            if (rssi <= -51 && rssi >= -113) {
                ss->LTE_SignalStrength.signalStrength = (rssi + 113) / 2;
                got_any = 1;
                got_lte = 1;
            }
            if (rsrp <= -44 && rsrp >= -140) {
                ss->LTE_SignalStrength.rsrp = -rsrp;
                got_any = 1;
                got_lte = 1;
            }
            if (rsrq <= -3 && rsrq >= -20) {
                ss->LTE_SignalStrength.rsrq = -rsrq;
                got_any = 1;
                got_lte = 1;
            }
            if (snr >= -200 && snr <= 300) {
                ss->LTE_SignalStrength.rssnr = snr;
                got_any = 1;
                got_lte = 1;
            }
        } else if (t == 0x17 && l >= 4) {
            /* 5G NR: int16 rsrp (dBm), int16 snr (0.1 dB); INT16_MIN = invalid */
            int16_t rsrp = (int16_t)(buf[pos] | (buf[pos + 1] << 8));
            int16_t snr = (int16_t)(buf[pos + 2] | (buf[pos + 3] << 8));
            if (rsrp != INT16_MIN && rsrp <= -44 && rsrp >= -140) {
                ss->NR_SignalStrength.ssRsrp = -rsrp;
                got_any = 1;
                got_nr = 1;
            }
            if (snr != INT16_MIN && snr >= -230 && snr <= 400) {
                ss->NR_SignalStrength.ssSinr = snr / 10;
                got_any = 1;
                got_nr = 1;
            }
        } else if (t == 0x18 && l >= 2) {
            /* 5G NR Extended: int16 rsrq (dB); INT16_MIN = invalid */
            int16_t rsrq = (int16_t)(buf[pos] | (buf[pos + 1] << 8));
            if (rsrq != INT16_MIN && rsrq <= -3 && rsrq >= -20) {
                ss->NR_SignalStrength.ssRsrq = -rsrq;
                got_any = 1;
                got_nr = 1;
            }
        }
        pos += l;
    }

    /*
     * During 4G <-> 5G SA handovers SDX55 can briefly report both LTE (0x14)
     * and NR (0x17) TLVs in GetSignalInfo. Keep only the serving RAT's entry
     * (unless on EN-DC NSA where act == 13 uses both LTE anchor + NR) so
     * Android does not oscillate between LTE and NR primary signal bars.
     */
    if (got_lte && got_nr && s_lastRegAct != 13) {
        if (s_lastRegTech == RADIO_TECH_NR) {
            ss->LTE_SignalStrength.signalStrength = 99;
            ss->LTE_SignalStrength.rsrp = INT_MAX;
            ss->LTE_SignalStrength.rsrq = INT_MAX;
            ss->LTE_SignalStrength.rssnr = INT_MAX;
            ss->LTE_SignalStrength.cqi = INT_MAX;
            ss->LTE_SignalStrength.timingAdvance = INT_MAX;
        } else if (s_lastRegTech == RADIO_TECH_LTE || s_lastRegTech == RADIO_TECH_LTE_CA) {
            ss->NR_SignalStrength.ssRsrp = INT_MAX;
            ss->NR_SignalStrength.ssRsrq = INT_MAX;
            ss->NR_SignalStrength.ssSinr = INT_MAX;
            ss->NR_SignalStrength.csiRsrp = INT_MAX;
            ss->NR_SignalStrength.csiRsrq = INT_MAX;
            ss->NR_SignalStrength.csiSinr = INT_MAX;
        }
    }

    return got_any ? 0 : -1;
}

// Fills a RIL_SignalStrength_v12-sized int array from QMI NAS (preferred for LTE/5G NR)
// or falls back to AT+CSQ. Returns 0 on success.
static int readSignalStrength(int *response, int maxNumOfElements)
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    int count = 0;
    // Accept a response that is at least v6, and up to v12
    int minNumOfElements=sizeof(RIL_SignalStrength_v6)/sizeof(int);
    RIL_SignalStrength_v12 ss;

    memset(response, 0, maxNumOfElements * sizeof(int));

    if (readQmiNasSignalStrength(&ss) == 0) {
        memcpy(response, &ss, sizeof(ss) < maxNumOfElements * sizeof(int)
                ? sizeof(ss) : maxNumOfElements * sizeof(int));
        return 0;
    }

    // TODO(b/206814247): Rename AT+CSQ command.
    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);
    if (err < 0 || p_response->success == 0) goto error;

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    for (count = 0; count < maxNumOfElements; count++) {
        err = at_tok_nextint(&line, &(response[count]));
        if (err < 0) break;
    }

    if (count < minNumOfElements) {
        // A real modem only gives <rssi>,<ber>; fill GSM, WCDMA and LTE from it
        int rssi, ber, rssi_dbm, rsrp;
        if (count < 2) goto error;
        rssi = response[0];
        ber = response[1];
        initEmptySignalStrength(&ss);
        ss.GW_SignalStrength.signalStrength = rssi;
        ss.GW_SignalStrength.bitErrorRate = ber;
        ss.WCDMA_SignalStrength.signalStrength = rssi;
        ss.WCDMA_SignalStrength.bitErrorRate = ber;
        ss.LTE_SignalStrength.signalStrength = rssi;
        if (rssi >= 0 && rssi <= 31) {
            // RSRP sits roughly 20 dB under the wideband RSSI on a loaded LTE carrier
            rssi_dbm = -113 + 2 * rssi;
            rsrp = -(rssi_dbm - 20);
            if (rsrp < 44) rsrp = 44;
            if (rsrp > 140) rsrp = 140;
            ss.LTE_SignalStrength.rsrp = rsrp;
        }
        memcpy(response, &ss, sizeof(ss) < maxNumOfElements * sizeof(int)
                ? sizeof(ss) : maxNumOfElements * sizeof(int));
    }

    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

static void requestSignalStrength(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int maxNumOfElements=sizeof(RIL_SignalStrength_v12)/sizeof(int);
    int response[maxNumOfElements];

    if (readSignalStrength(response, maxNumOfElements) < 0) {
        RLOGE("requestSignalStrength must never return an error when radio is on");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
}

// A real modem never reports +CSQ on its own, so push signal strength periodically
static const struct timeval TIMEVAL_SIGNALPOLL = {3, 0};
static int s_signalPollStarted = 0;

static void pushSignalStrengthOnce(void *param __unused)
{
    int maxNumOfElements = sizeof(RIL_SignalStrength_v12) / sizeof(int);
    int response[maxNumOfElements];

    if (sState == RADIO_STATE_ON &&
            readSignalStrength(response, maxNumOfElements) == 0) {
        memcpy(s_lastSignal, response, sizeof(response));
        s_lastSignalValid = 1;
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, response, sizeof(response));
    }
}

static void notifyNetworkAndSignal(void *param __unused)
{
    if (sState != RADIO_STATE_ON) return;
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
    pushSignalStrengthOnce(NULL);
}

static void pollSignalStrength(void *param __unused)
{
    int maxNumOfElements = sizeof(RIL_SignalStrength_v12) / sizeof(int);
    int response[maxNumOfElements];

    if (sState == RADIO_STATE_ON &&
            readSignalStrength(response, maxNumOfElements) == 0) {
        if (!s_lastSignalValid || memcmp(s_lastSignal, response, sizeof(response)) != 0) {
            memcpy(s_lastSignal, response, sizeof(response));
            s_lastSignalValid = 1;
            RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, response, sizeof(response));
        }
    }
    RIL_requestTimedCallback(pollSignalStrength, NULL, &TIMEVAL_SIGNALPOLL);
}

/**
 * networkModePossible. Decides whether the network mode is appropriate for the
 * specified modem
 */
static int networkModePossible(ModemInfo *mdm, int nm)
{
    const int asize = sizeof(net2modem) / sizeof(net2modem[0]);
    if (nm >= asize || nm < 0) {
        RLOGW("%s %d: invalid net2modem index: %d", __func__, __LINE__, nm);
        return 0;
    }
    if ((net2modem[nm] & mdm->supportedTechs) == net2modem[nm]) {
       return 1;
    }
    return 0;
}

int getPreferredFromBitmap(int value, int *index) {
    for (unsigned int i = 0; i < sizeof(s_networkMask) / sizeof(NetworkTypeBitmap); i++) {
        if (s_networkMask[i].bitmap == value) {
            if (index) *index = i;
            return s_networkMask[i].type;
        }
    }
    // set default value here, since there is no match found
    // ref.
    //{LTE | GSM | WCDMA,               MDM_LTE | MDM_GSM | MDM_WCDMA},             // 9 - LTE, GSM/WCDMA
    //
    const int DEFAULT_PREFERRED_INDEX = 9;
    const int DEFAULT_PREFERRED_BITMAP = MDM_LTE | MDM_GSM | MDM_WCDMA;
    assert(s_networkMask[DEFAULT_PREFERRED_INDEX] == DEFAULT_PREFERRED_BITMAP);
    if (index) {
        *index = DEFAULT_PREFERRED_INDEX;
    }
    RLOGD("getPreferredFromBitmap %d not match", value);
    return  DEFAULT_PREFERRED_BITMAP;
}

unsigned getBitmapFromPreferred(int value) {
    for (unsigned int i = 0; i < sizeof(s_networkMask) / sizeof(NetworkTypeBitmap); i++) {
        if (s_networkMask[i].type == value) {
            return s_networkMask[i].bitmap;
        }
    }
    RLOGD("getBitmapFromPreferred %d not match", value);
    return  LTE | GSM | WCDMA;
}

// The modem picks its RATs itself (NAS mode preference, 5G first); remember what
// Android allows so it reads it back and keeps NR enabled.
static int s_allowedNetworkTypes = -1;

static void requestSetPreferredNetworkType(int request, void *data,
                                           size_t datalen __unused, RIL_Token t )
{
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    int value = *(int *)data;
    int index = value;
    int current, old;
    int err;
    int32_t preferred;

    if (request != RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE) {
        s_allowedNetworkTypes = value;
        s_lastSignalValid = 0;
        if (qmiNasSetModePreference(value) == 0) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
            RIL_requestTimedCallback(pushSignalStrengthOnce, NULL, &TIMEVAL_SIGNAL_IMMEDIATE);
            RIL_requestTimedCallback(notifyNetworkAndSignal, NULL, &TIMEVAL_RAT_SETTLE);
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
    }

    if (request == RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE) {
        preferred = net2pmask[value];
    } else {
        preferred = getPreferredFromBitmap(value, &index);
    }
    RLOGD("requestSetPreferredNetworkType: current: %x. New: %x", PREFERRED_NETWORK(sMdmInfo), preferred);

    if (!networkModePossible(sMdmInfo, index)) {
        RIL_onRequestComplete(t, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
        return;
    }

    if (query_ctec(sMdmInfo, &current, NULL) < 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    old = PREFERRED_NETWORK(sMdmInfo);
    RLOGD("old != preferred: %d", old != preferred);
    if (old != preferred) {
        asprintf(&cmd, "AT+CTEC=%d,\"%x\"", current, preferred);
        RLOGD("Sending command: <%s>", cmd);
        err = at_send_command_singleline(cmd, "+CTEC:", &p_response);
        free(cmd);
        if (err || !p_response->success) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }
        PREFERRED_NETWORK(sMdmInfo) = value;
        if (!strstr( p_response->p_intermediates->line, "DONE") ) {
            int current;
            int res = parse_technology_response(p_response->p_intermediates->line, &current, NULL);
            switch (res) {
                case -1: // Error or unable to parse
                    break;
                case 1: // Only able to parse current
                case 0: // Both current and preferred were parsed
                    setRadioTechnology(sMdmInfo, current);
                    break;
            }
        }
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetPreferredNetworkType(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    if (s_allowedNetworkTypes >= 0 &&
            (request == RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP ||
             request == RIL_REQUEST_GET_ALLOWED_NETWORK_TYPES_BITMAP)) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_allowedNetworkTypes, sizeof(s_allowedNetworkTypes));
        return;
    }

    int preferred;
    unsigned i;

    switch ( query_ctec(sMdmInfo, NULL, &preferred) ) {
        case -1: // Error or unable to parse
        case 1: // Only able to parse current
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            break;
        case 0: // Both current and preferred were parsed
            for ( i = 0 ; i < sizeof(net2pmask) / sizeof(int32_t) ; i++ ) {
                if (preferred == net2pmask[i]) {
                    goto done;
                }
            }
            RLOGE("Unknown preferred mode received from modem: %d", preferred);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
    }
done:
    if (request == RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP ||
            request == RIL_REQUEST_GET_ALLOWED_NETWORK_TYPES_BITMAP) {
        i = getBitmapFromPreferred(preferred);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &i, sizeof(i));
}

static void requestGetCarrierRestrictions(void* data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    RIL_UNUSED_PARM(data);

    // Fixed values. TODO: query modem
    RIL_Carrier allowed_carriers = {
            "123",          // mcc
            "456",          // mnc
            RIL_MATCH_ALL,  // match_type
            "",             // match_data
    };

    RIL_Carrier excluded_carriers;

    RIL_CarrierRestrictionsWithPriority restrictions = {
            1,                   // len_allowed_carriers
            0,                   // len_excluded_carriers
            &allowed_carriers,   // allowed_carriers
            &excluded_carriers,  // excluded_carriers
            1,                   // allowedCarriersPrioritized
            NO_MULTISIM_POLICY   // multiSimPolicy
    };

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &restrictions, sizeof(restrictions));
}

static void requestCdmaPrlVersion(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    int err;
    char * responseStr;
    ATResponse *p_response = NULL;
    const char *cmd;
    char *line;

    err = at_send_command_singleline("AT+WPRL?", "+WPRL:", &p_response);
    if (err < 0 || !p_response->success) goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextstr(&line, &responseStr);
    if (err < 0 || !responseStr) goto error;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, strlen(responseStr));
    at_response_free(p_response);
    return;
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaBaseBandVersion(int request __unused, void *data __unused,
                                   size_t datalen __unused, RIL_Token t)
{
    char ver[256];

    if (qmiDmsGetBasebandVersion(ver, sizeof(ver)) == 0) {
        RLOGI("Baseband version (QMI DMS): %s", ver);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, ver, strlen(ver) + 1);
        return;
    }

    ATResponse *p_response = NULL;
    int err = at_send_command_multiline("AT+CGMR", "", &p_response);
    if (err >= 0 && p_response != NULL && p_response->success != 0 &&
        p_response->p_intermediates != NULL && p_response->p_intermediates->line != NULL) {
        const char *line = p_response->p_intermediates->line;
        if (strncmp(line, "+CGMR:", 6) == 0) line += 6;
        while (*line == ' ') line++;
        snprintf(ver, sizeof(ver), "%s", line);
        at_response_free(p_response);
        RLOGI("Baseband version (AT+CGMR): %s", ver);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, ver, strlen(ver) + 1);
        return;
    }
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestDeviceIdentityForSlot(int slot_id, RIL_Token t)
{
    char imei[32] = {0};
    char imeisv[16] = {0};
    char esn[32] = {0};
    char meid[32] = {0};
    char *responseStr[4];
    ATResponse *p_response = NULL;

    if (qmiDmsGetSerialNumbersForSlot(slot_id, imei, sizeof(imei),
                                      imeisv, sizeof(imeisv),
                                      esn, sizeof(esn),
                                      meid, sizeof(meid)) < 0) {
        if (slot_id != 0) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }
        int err = at_send_command_numeric("AT+CGSN", &p_response);
        if (err < 0 || p_response == NULL || p_response->success == 0 ||
            p_response->p_intermediates == NULL) {
            at_response_free(p_response);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }
        snprintf(imei, sizeof(imei), "%s", p_response->p_intermediates->line);
        at_response_free(p_response);
    }

    RLOGI("DeviceIdentity(slot=%d): imei=%s imeisv=%s", slot_id, imei, imeisv);

    responseStr[0] = imei;
    responseStr[1] = imeisv;
    responseStr[2] = esn;
    responseStr[3] = meid;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, 4 * sizeof(char *));
}

static void requestDeviceIdentity(int request __unused, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    requestDeviceIdentityForSlot(0, t);
}

static void requestCdmaGetSubscriptionSource(int request __unused, void *data,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;
    char *line = NULL;
    int response;

    asprintf(&cmd, "AT+CCSS?");
    if (!cmd) goto error;

    err = at_send_command_singleline(cmd, "+CCSS:", &p_response);
    if (err < 0 || !p_response->success)
        goto error;

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetSubscriptionSource(int request __unused, void *data,
                                        size_t datalen, RIL_Token t)
{
    int err;
    int *ss = (int *)data;
    ATResponse *p_response = NULL;
    char *cmd = NULL;

    if (!ss || !datalen) {
        RLOGE("RIL_REQUEST_CDMA_SET_SUBSCRIPTION without data!");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    asprintf(&cmd, "AT+CCSS=%d", ss[0]);
    if (!cmd) goto error;

    err = at_send_command(cmd, &p_response);
    if (err < 0 || !p_response->success)
        goto error;
    free(cmd);
    cmd = NULL;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED, ss, sizeof(ss[0]));

    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSubscription(int request __unused, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int response[5];
    char * responseStr[5];
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line, *p;
    int commas;
    int skip;
    int count = 5;

    // Fixed values. TODO: Query modem
    responseStr[0] = "8587777777"; // MDN
    responseStr[1] = "1"; // SID
    responseStr[2] = "1"; // NID
    responseStr[3] = "8587777777"; // MIN
    responseStr[4] = "1"; // PRL Version
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count*sizeof(char*));
}

static void requestCdmaGetRoamingPreference(int request __unused, void *data __unused,
                                                 size_t datalen __unused, RIL_Token t)
{
    int roaming_pref = -1;
    ATResponse *p_response = NULL;
    char *line;
    int res;

    res = at_send_command_singleline("AT+WRMP?", "+WRMP:", &p_response);
    if (res < 0 || !p_response->success) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    res = at_tok_start(&line);
    if (res < 0) goto error;

    res = at_tok_nextint(&line, &roaming_pref);
    if (res < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &roaming_pref, sizeof(roaming_pref));
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestCdmaSetRoamingPreference(int request __unused, void *data,
                                                 size_t datalen __unused, RIL_Token t)
{
    int *pref = (int *)data;
    ATResponse *p_response = NULL;
    char *line;
    int res;
    char *cmd = NULL;

    asprintf(&cmd, "AT+WRMP=%d", *pref);
    if (cmd == NULL) goto error;

    res = at_send_command(cmd, &p_response);
    if (res < 0 || !p_response->success)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    free(cmd);
    return;
error:
    free(cmd);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static uint64_t s_cid64 = 0;

static int parseRegistrationState(char *str, int *type, int *items, int **response)
{
    int err;
    char *line = str, *p;
    int *resp = NULL;
    int skip;
    int commas;
    int out_items = 0;

    s_lac = -1;
    s_cid = -1;
    s_cid64 = 0;

    RLOGD("parseRegistrationState. Parsing: %s",str);
    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* count number of commas */
    commas = 0;
    for (p = line ; *p != '\0' ;p++) {
        if (*p == ',') commas++;
    }

    resp = (int *)calloc(commas + 4, sizeof(int));
    if (!resp) goto error;
    switch (commas) {
        case 0: /* +CREG: <stat> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            out_items = 1;
            break;

        case 1: /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            out_items = 1;
            break;

        case 2: /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            {
                char *cid_str = NULL;
                err = at_tok_nextstr(&line, &cid_str);
                if (err < 0 || !cid_str) goto error;
                s_cid64 = strtoull(cid_str, NULL, 16);
                resp[2] = (int)s_cid64;
            }
            out_items = 3;
            break;

        case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            {
                char *cid_str = NULL;
                err = at_tok_nextstr(&line, &cid_str);
                if (err < 0 || !cid_str) goto error;
                s_cid64 = strtoull(cid_str, NULL, 16);
                resp[2] = (int)s_cid64;
            }
            out_items = 3;
            break;

        default: /* +CGREG/+CEREG/+C5GREG: <n>, <stat>, <lac/tac>, <cid/nci>, <AcT> [, ...] */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            {
                char *cid_str = NULL;
                err = at_tok_nextstr(&line, &cid_str);
                if (err < 0 || !cid_str) goto error;
                s_cid64 = strtoull(cid_str, NULL, 16);
                resp[2] = (int)s_cid64;
            }
            err = at_tok_nextint(&line, &resp[3]);
            if (err < 0) goto error;
            out_items = 4;
            break;
    }

    if (out_items >= 3) {
        s_lac = resp[1];
        s_cid = resp[2];
    }

    if (response)
        *response = resp;
    if (items)
        *items = out_items;
    if (type)
        *type = techFromModemType(TECH(sMdmInfo));
    return 0;
error:
    free(resp);
    return -1;
}

static int mapNetworkRegistrationResponse(int in_response) {
    int out_response = 0;

    switch (in_response) {
        case 0:
            out_response = RADIO_TECH_GPRS;    /* GPRS */
            break;
        case 3:
            out_response = RADIO_TECH_EDGE;    /* EDGE */
            break;
        case 2:
            out_response = RADIO_TECH_UMTS;    /* TD */
            break;
        case 4:
            out_response = RADIO_TECH_HSDPA;   /* HSDPA */
            break;
        case 5:
            out_response = RADIO_TECH_HSUPA;   /* HSUPA */
            break;
        case 6:
            out_response = RADIO_TECH_HSPA;    /* HSPA */
            break;
        case 15:
            out_response = RADIO_TECH_HSPAP;   /* HSPA+ */
            break;
        case 7:
            out_response = RADIO_TECH_LTE;     /* LTE */
            break;
        case 16:
            out_response = RADIO_TECH_LTE_CA;  /* LTE_CA */
            break;
        case 13:  // E-UTRA-NR dual connectivity: NSA, LTE anchor reported with EN-DC
            out_response = RADIO_TECH_LTE;
            break;
        case 11:  // NR connected to a 5GCN
        case 12:  // NG-RAN
            out_response = RADIO_TECH_NR;      /* NR */
            break;
        default:
            out_response = RADIO_TECH_UNKNOWN; /* UNKNOWN */
            break;
    }
    return out_response;
}

#define REG_STATE_LEN 18
#define REG_DATA_STATE_LEN 15
// Access technology of the serving cell from +COPS? (3GPP TS 27.007 <AcT>)
static int getCopsRadioTech(int *act_out) {
    ATResponse *p_response = NULL;
    int tech = RADIO_TECH_UNKNOWN;
    int mode, format, act = -1;
    char *line, *oper;

    if (at_send_command_singleline("AT+COPS?", "+COPS:", &p_response) < 0 ||
            !p_response->success) {
        goto done;
    }
    line = p_response->p_intermediates->line;
    if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &mode) < 0 ||
            !at_tok_hasmore(&line) || at_tok_nextint(&line, &format) < 0 ||
            at_tok_nextstr(&line, &oper) < 0 || !at_tok_hasmore(&line) ||
            at_tok_nextint(&line, &act) < 0) {
        goto done;
    }
    switch (act) {
        case 0: case 1: tech = RADIO_TECH_GSM; break;
        case 2: tech = RADIO_TECH_UMTS; break;
        case 3: tech = RADIO_TECH_EDGE; break;
        case 4: tech = RADIO_TECH_HSDPA; break;
        case 5: tech = RADIO_TECH_HSUPA; break;
        case 6: tech = RADIO_TECH_HSPA; break;
        case 7: case 9: case 13: tech = RADIO_TECH_LTE; break;
        case 10: case 11: case 12: tech = RADIO_TECH_NR; break;
    }
done:
    if (act_out) *act_out = act;
    at_response_free(p_response);
    return tech;
}

static void requestRegistrationState(int request, void *data __unused,
                                        size_t datalen __unused, RIL_Token t)
{
    int err;
    int *registration;
    char **responseStr = NULL;
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type, startfrom;
    int act = -1;

    RLOGD("requestRegistrationState");
    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
        numElements = REG_STATE_LEN;
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        cmd = "AT+CGREG?";
        prefix = "+CGREG:";
        numElements = REG_DATA_STATE_LEN;
        if (TECH_BIT(sMdmInfo) == MDM_LTE) {
            cmd = "AT+CEREG?";
            prefix = "+CEREG:";
        }
    } else {
        assert(0);
        goto error;
    }

    // If UICC applications are disabled ("Use this SIM" toggled OFF), report
    // unregistered immediately so Android drops service without waiting on AT+COPS=2.
    if (!areUiccApplicationsEnabled) {
        s_lastRegTech = RADIO_TECH_UNKNOWN;
        s_lastRegAct = -1;
        responseStr = calloc(numElements, sizeof(char *));
        if (!responseStr) goto error;
        asprintf(&responseStr[0], "0");
        if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
            asprintf(&responseStr[11], "%d", s_mcc);
            asprintf(&responseStr[12], "%d", s_mnc);
            if (s_mncLength == 2) {
                asprintf(&responseStr[13], "%03d%02d", s_mcc, s_mnc);
            } else {
                asprintf(&responseStr[13], "%03d%03d", s_mcc, s_mnc);
            }
            asprintf(&responseStr[14], "0");
        } else {
            asprintf(&responseStr[15], "%d", s_mcc);
            asprintf(&responseStr[16], "%d", s_mnc);
            if (s_mncLength == 2) {
                asprintf(&responseStr[17], "%03d%02d", s_mcc, s_mnc);
            } else {
                asprintf(&responseStr[17], "%03d%03d", s_mcc, s_mnc);
            }
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, numElements * sizeof(responseStr));
        for (j = 0; j < numElements; j++) free(responseStr[j]);
        free(responseStr);
        return;
    }

    // Self-heal if SDX55 ever reverted +C*REG URC mode <n> back to 0 (no TAC/NCI/AcT or URCs).
    int rearmed_urc = 0;
    int got_reg = 0;
    uint64_t cid64 = 0;
    type = 0;

    // On 5G SA / LTE networks (e.g. Jio), prefer active 5GS (+C5GREG?) first when NR is allowed,
    // then EPS (+CEREG?), then legacy (+CGREG? / +CREG?) so 4G->5G handovers are never masked
    // by a transient LTE registration.
    int nr_allowed = (s_allowedNetworkTypes < 0) || ((s_allowedNetworkTypes & NR) != 0);
    if (nr_allowed) {
        ATResponse *p_nr = NULL;
        int *nr = NULL, nr_type = 0, nr_count = 3;
        err = at_send_command_singleline("AT+C5GREG?", "+C5GREG:", &p_nr);
        if (err == 0 && p_nr && p_nr->success) {
            char *nr_line = p_nr->p_intermediates->line;
            if (strstr(nr_line, ": 0,") && !rearmed_urc) {
                rearmed_urc = 1;
                at_response_free(p_nr);
                p_nr = NULL;
                enableNetworkRegistrationUrks();
                err = at_send_command_singleline("AT+C5GREG?", "+C5GREG:", &p_nr);
                nr_line = (err == 0 && p_nr && p_nr->success) ? p_nr->p_intermediates->line : NULL;
            }
            if (nr_line && !parseRegistrationState(nr_line, &nr_type, &nr_count, &nr) &&
                    (nr[0] == 1 || nr[0] == 5)) {
                registration = nr;
                nr = NULL;
                type = nr_type;
                count = nr_count;
                cid64 = s_cid64;
                got_reg = 1;
            }
        }
        free(nr);
        at_response_free(p_nr);
    }

    if (!got_reg) {
        ATResponse *p_eps = NULL;
        int *eps = NULL, eps_type = 0, eps_count = 3;
        err = at_send_command_singleline("AT+CEREG?", "+CEREG:", &p_eps);
        if (err == 0 && p_eps && p_eps->success) {
            char *eps_line = p_eps->p_intermediates->line;
            if (strstr(eps_line, ": 0,") && !rearmed_urc) {
                rearmed_urc = 1;
                at_response_free(p_eps);
                p_eps = NULL;
                enableNetworkRegistrationUrks();
                err = at_send_command_singleline("AT+CEREG?", "+CEREG:", &p_eps);
                eps_line = (err == 0 && p_eps && p_eps->success) ? p_eps->p_intermediates->line : NULL;
            }
            if (eps_line && !parseRegistrationState(eps_line, &eps_type, &eps_count, &eps) &&
                    (eps[0] == 1 || eps[0] == 5)) {
                registration = eps;
                eps = NULL;
                type = eps_type;
                count = eps_count;
                cid64 = s_cid64;
                got_reg = 1;
            }
        }
        free(eps);
        at_response_free(p_eps);
    }

    if (!got_reg) {
        err = at_send_command_singleline(cmd, prefix, &p_response);
        if (err < 0 || !p_response->success) goto error;
        line = p_response->p_intermediates->line;
        if (parseRegistrationState(line, &type, &count, &registration)) goto error;
        cid64 = s_cid64;
    }

    responseStr = malloc(numElements * sizeof(char *));
    if (!responseStr) goto error;
    memset(responseStr, 0, numElements * sizeof(char *));
    /**
     * The first '4' bytes for both registration states remain the same.
     * But if the request is 'DATA_REGISTRATION_STATE',
     * the 5th and 6th byte(s) are optional.
     */
    if (is3gpp2(type) == 1) {
        RLOGD("registration state type: 3GPP2");
        // TODO: Query modem
        startfrom = 3;
        if(request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");     // EvDo revA
            asprintf(&responseStr[4], "1");     // BSID
            asprintf(&responseStr[5], "123");   // Latitude
            asprintf(&responseStr[6], "222");   // Longitude
            asprintf(&responseStr[7], "0");     // CSS Indicator
            asprintf(&responseStr[8], "4");     // SID
            asprintf(&responseStr[9], "65535"); // NID
            asprintf(&responseStr[10], "0");    // Roaming indicator
            asprintf(&responseStr[11], "1");    // System is in PRL
            asprintf(&responseStr[12], "0");    // Default Roaming indicator
            asprintf(&responseStr[13], "0");    // Reason for denial
            asprintf(&responseStr[14], "0");    // Primary Scrambling Code of Current cell
      } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
            asprintf(&responseStr[3], "8");   // Available data radio technology
      }
    } else { // type == RADIO_TECH_3GPP
        RLOGD("registration state type: 3GPP");
        startfrom = 0;
        if (count > 1) {
            asprintf(&responseStr[1], "%x", registration[1]);
        }
        if (count > 2) {
            asprintf(&responseStr[2], "%" PRIx64, cid64);
        }
        if (count > 3) {
            int tech = mapNetworkRegistrationResponse(registration[3]);
            act = registration[3];
            asprintf(&responseStr[3], "%d", tech);
            if (registration[0] == 1 || registration[0] == 5) {
                int prevTech = s_lastRegTech;
                s_lastRegTech = tech;
                s_lastRegAct = act;
                if (prevTech != tech) {
                    s_lastSignalValid = 0;
                    RIL_requestTimedCallback(pushSignalStrengthOnce, NULL, &TIMEVAL_SIGNAL_IMMEDIATE);
                }
            } else {
                s_lastRegTech = RADIO_TECH_UNKNOWN;
                s_lastRegAct = -1;
            }
        } else if (registration[0] == 1 || registration[0] == 5) {
            int tech = getCopsRadioTech(&act);
            if (tech != RADIO_TECH_UNKNOWN) {
                int prevTech = s_lastRegTech;
                asprintf(&responseStr[3], "%d", tech);
                s_lastRegTech = tech;
                s_lastRegAct = act;
                if (prevTech != tech) {
                    s_lastSignalValid = 0;
                    RIL_requestTimedCallback(pushSignalStrengthOnce, NULL, &TIMEVAL_SIGNAL_IMMEDIATE);
                }
            }
        } else {
            s_lastRegTech = RADIO_TECH_UNKNOWN;
            s_lastRegAct = -1;
        }
    }
    asprintf(&responseStr[0], "%d", registration[0]);

    /**
     * Optional bytes for DATA_REGISTRATION_STATE request
     * 4th byte : Registration denial code
     * 5th byte : The max. number of simultaneous Data Calls
     */
    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        // asprintf(&responseStr[4], "3");
        // asprintf(&responseStr[5], "1");
        asprintf(&responseStr[11], "%d", s_mcc);
        asprintf(&responseStr[12], "%d", s_mnc);
        if (s_mncLength == 2) {
          asprintf(&responseStr[13], "%03d%02d", s_mcc, s_mnc);
        } else {
          asprintf(&responseStr[13], "%03d%03d", s_mcc, s_mnc);
        }
        // EN-DC available: libril reports it in the E-UTRAN NR indicators
        if (act < 0 && (registration[0] == 1 || registration[0] == 5)) getCopsRadioTech(&act);
        asprintf(&responseStr[14], "%d", act == 13);
    } else {  // Voice
        asprintf(&responseStr[15], "%d", s_mcc);
        asprintf(&responseStr[16], "%d", s_mnc);
        if (s_mncLength == 2) {
          asprintf(&responseStr[17], "%03d%02d", s_mcc, s_mnc);
        } else {
          asprintf(&responseStr[17], "%03d%03d", s_mcc, s_mnc);
        }
    }


    for (j = startfrom; j < numElements; j++) {
        if (!responseStr[i]) goto error;
    }
    free(registration);
    registration = NULL;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, numElements*sizeof(responseStr));
    for (j = 0; j < numElements; j++ ) {
        free(responseStr[j]);
        responseStr[j] = NULL;
    }
    free(responseStr);
    responseStr = NULL;
    at_response_free(p_response);

    return;
error:
    if (responseStr) {
        for (j = 0; j < numElements; j++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }
        free(responseStr);
        responseStr = NULL;
    }
    RLOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestOperator(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));

    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
        // Simple assumption that mcc and mnc are 3 digits each
        int length = strlen(response[i]);
        if (length == 6) {
            s_mncLength = 3;
            if (sscanf(response[i], "%3d%3d", &s_mcc, &s_mnc) != 2) {
                RLOGE("requestOperator expected mccmnc to be 6 decimal digits");
            }
        } else if (length == 5) {
            s_mncLength = 2;
            if (sscanf(response[i], "%3d%2d", &s_mcc, &s_mnc) != 2) {
                RLOGE("requestOperator expected mccmnc to be 5 decimal digits");
            }
        }
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    RLOGE("requestOperator must not return error when radio is on");
    s_mncLength = 0;
    s_mcc = 0;
    s_mnc = 0;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestCdmaSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err = 1; // Set to go to error:
    RIL_SMS_Response response;
    RIL_CDMA_SMS_Message* rcsm;

    memset(&response, 0, sizeof(response));

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    RLOGD("requestCdmaSendSMS datalen=%zu, sizeof(RIL_CDMA_SMS_Message)=%zu",
            datalen, sizeof(RIL_CDMA_SMS_Message));

    // verify data content to test marshalling/unmarshalling:
    rcsm = (RIL_CDMA_SMS_Message*)data;
    RLOGD("TeleserviceID=%d, bIsServicePresent=%d, \
            uServicecategory=%d, sAddress.digit_mode=%d, \
            sAddress.Number_mode=%d, sAddress.number_type=%d, ",
            rcsm->uTeleserviceID,  rcsm->bIsServicePresent,
            rcsm->uServicecategory,rcsm->sAddress.digit_mode,
            rcsm->sAddress.number_mode,rcsm->sAddress.number_type);

    if (err != 0) goto error;

    // Cdma Send SMS implementation will go here:
    // But it is not implemented yet.

    response.messageRef = 1;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    // Cdma Send SMS will always cause send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

static void requestSendSMS(void *data, size_t datalen, RIL_Token t)
{
    int err;
    const char *smsc;
    const char *pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    memset(&response, 0, sizeof(response));
    RLOGD("requestSendSMS datalen =%zu", datalen);

    if (s_ims_gsm_fail != 0) goto error;
    if (s_ims_gsm_retry != 0) goto error2;

    smsc = ((const char **)data)[0];
    pdu = ((const char **)data)[1];

    tpLayerLength = strlen(pdu)/2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc= "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    free(cmd1);
    free(cmd2);

    if (err != 0 || p_response->success == 0) goto error;

    int messageRef = 1;
    char *line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &messageRef);
    if (err < 0) goto error;

    /* FIXME fill in ackPDU */
    response.messageRef = messageRef;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;
error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    at_response_free(p_response);
    return;
error2:
    // send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
    at_response_free(p_response);
    return;
}

static void requestImsSendSMS(void *data, size_t datalen, RIL_Token t)
{
    RIL_IMS_SMS_Message *p_args;
    RIL_SMS_Response response;

    memset(&response, 0, sizeof(response));

    RLOGD("requestImsSendSMS: datalen=%zu, "
        "registered=%d, service=%d, format=%d, ims_perm_fail=%d, "
        "ims_retry=%d, gsm_fail=%d, gsm_retry=%d",
        datalen, s_ims_registered, s_ims_services, s_ims_format,
        s_ims_cause_perm_failure, s_ims_cause_retry, s_ims_gsm_fail,
        s_ims_gsm_retry);

    // figure out if this is gsm/cdma format
    // then route it to requestSendSMS vs requestCdmaSendSMS respectively
    p_args = (RIL_IMS_SMS_Message *)data;

    if (0 != s_ims_cause_perm_failure ) goto error;

    // want to fail over ims and this is first request over ims
    if (0 != s_ims_cause_retry && 0 == p_args->retry) goto error2;

    if (RADIO_TECH_3GPP == p_args->tech) {
        return requestSendSMS(p_args->message.gsmMessage,
                datalen - sizeof(RIL_RadioTechnologyFamily),
                t);
    } else if (RADIO_TECH_3GPP2 == p_args->tech) {
        return requestCdmaSendSMS(p_args->message.cdmaMessage,
                datalen - sizeof(RIL_RadioTechnologyFamily),
                t);
    } else {
        RLOGE("requestImsSendSMS invalid format value =%d", p_args->tech);
    }

error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    return;

error2:
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

/**
 * Add for CTS test
 * If open logical channel with AID NULL, this means open logical channel to MF.
 * If there is P2 value, this P2 value is used for SELECT command.
 * In addition, if SELECT command returns 61xx, GET RESPONSE command needs to send to get data.
 */
static int sendCmdAgainForOpenChannelWithP2( char *data,
                                        int p2, int *response, int *rspLen) {
    int len = 0;
    int err = -1;
    char *line = NULL;
    char cmd[64] = {0};
    RIL_Errno errType = RIL_E_GENERIC_FAILURE;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));
    sscanf(data, "%2x", &(response[0]));  // response[0] is channel number

    // Send SELECT command to MF
    snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,14,00A400%02X023F00", response[0],
             p2);

    err = at_send_command_singleline(cmd, "+CGLA:", &p_response);
    if (err < 0) goto done;
    if (p_response->success == 0) {
        if (!strcmp(p_response->finalResponse, "+CME ERROR: 21") ||
            !strcmp(p_response->finalResponse, "+CME ERROR: 50")) {
            errType = RIL_E_GENERIC_FAILURE;
        }
        goto done;
    }

    line = p_response->p_intermediates->line;

    if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &len) < 0 ||
        at_tok_nextstr(&line, &(sr.simResponse)) < 0) {
        goto done;
    }

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));

    if (sr.sw1 == 0x90 && sr.sw2 == 0x00) {  // 9000 is successful
        int length = len / 2;
        for (*rspLen = 1; *rspLen <= length; (*rspLen)++) {
            sscanf(sr.simResponse, "%02x", &(response[*rspLen]));
            sr.simResponse += 2;
        }
        errType = RIL_E_SUCCESS;
    } else {  // close channel
        snprintf(cmd, sizeof(cmd), "AT+CCHC=%d", response[0]);
        at_send_command( cmd, NULL);
    }

done:
    at_response_free(p_response);
    return errType;
}

static void requestSimOpenChannel(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);

    int response[264] = {0};
    int responseLen = 0;
    RIL_OpenChannelParams *params = (RIL_OpenChannelParams *)data;

    if (!params) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    uint8_t *aid_bytes = NULL;
    size_t aid_len = 0;
    if (params->aidPtr != NULL && params->aidPtr[0] != '\0') {
        size_t hex_len = strlen(params->aidPtr);
        aid_len = hex_len / 2;
        aid_bytes = convertHexStringToBytes((void *)params->aidPtr, hex_len);
    }

    uint8_t ch = 0, sw1 = 0x90, sw2 = 0x00;
    uint8_t sel_resp[256];
    size_t sel_len = 0;
    int qret = qmiUimOpenLogicalChannel(aid_bytes, aid_len, params->p2,
                                        &ch, sel_resp, &sel_len, &sw1, &sw2);
    free(aid_bytes);
    if (qret == 0) {
        response[responseLen++] = ch;
        for (size_t i = 0; i < sel_len && responseLen < 260; i++) {
            response[responseLen++] = sel_resp[i];
        }
        response[responseLen++] = sw1;
        response[responseLen++] = sw2;
        RLOGD("requestSimOpenChannel(QMI UIM): aid=%s -> ch=%u sel_len=%zu sw=%02x%02x",
              params->aidPtr ? params->aidPtr : "(null)", ch, sel_len, sw1, sw2);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, response, responseLen * sizeof(int));
        return;
    }

    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSimCloseChannel(void *data, size_t datalen, RIL_Token t)
{
    int32_t session_id;

    if (data == NULL || datalen != sizeof(session_id)) {
        ALOGE("Invalid data passed to requestSimCloseChannel");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    session_id = ((int32_t *)data)[0];
    if (session_id <= 0 || session_id > 255) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    if (qmiUimCloseLogicalChannel((uint8_t)session_id) == 0) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
    }
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSimTransmitApduChannel(void *data,
                                          size_t datalen,
                                          RIL_Token t)
{
    RIL_SIM_IO_Response sr = {0};
    RIL_SIM_APDU *apdu = (RIL_SIM_APDU *)data;

    if (apdu == NULL || datalen != sizeof(RIL_SIM_APDU)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    uint8_t raw[300];
    size_t raw_len = 0;
    raw[raw_len++] = (uint8_t)(apdu->cla & 0xff);
    raw[raw_len++] = (uint8_t)(apdu->instruction & 0xff);
    raw[raw_len++] = (uint8_t)(apdu->p1 & 0xff);
    raw[raw_len++] = (uint8_t)(apdu->p2 & 0xff);
    if (apdu->p3 >= 0) {
        raw[raw_len++] = (uint8_t)(apdu->p3 & 0xff);
    }
    if (apdu->data != NULL && apdu->data[0] != '\0') {
        size_t hex_len = strlen(apdu->data);
        size_t dlen = hex_len / 2;
        uint8_t *dbytes = convertHexStringToBytes((void *)apdu->data, hex_len);
        if (!dbytes || raw_len + dlen > sizeof(raw)) {
            free(dbytes);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }
        memcpy(raw + raw_len, dbytes, dlen);
        raw_len += dlen;
        free(dbytes);
    }

    uint8_t resp[512];
    size_t resp_len = 0;
    if (qmiUimSendApdu((uint8_t)apdu->sessionid, raw, raw_len,
                       resp, sizeof(resp), &resp_len) < 0 || resp_len < 2) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    sr.sw1 = resp[resp_len - 2];
    sr.sw2 = resp[resp_len - 1];
    size_t payload_len = resp_len - 2;
    char *hexResp = (char *)calloc(payload_len * 2 + 1, 1);
    if (!hexResp) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }
    if (payload_len > 0) {
        convertBytesToHexString((char *)resp, (int)payload_len, (unsigned char *)hexResp);
    }
    sr.simResponse = hexResp;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    free(hexResp);
}

static void requestSimAuthentication(int authContext, char* authData, RIL_Token t) {
    unsigned char *binAuthData = NULL;
    RIL_SIM_IO_Response response;
    uint8_t ch = 0;
    int ch_opened = 0;

    memset(&response, 0, sizeof(response));
    response.sw1 = 0x90;
    response.sw2 = 0;

    if (!authData) goto error;

    binAuthData = (unsigned char *)malloc(strlen(authData) + 1);
    if (!binAuthData) goto error;
    int binAuthLen = base64_decode(authData, binAuthData);
    if (binAuthLen <= 0 || binAuthLen > 240) {
        RLOGE("requestSimAuthentication: base64_decode failed len=%d", binAuthLen);
        goto error;
    }

    /* Open logical channel via QMI UIM to ISIM (A0000000871004), falling back to USIM */
    static const uint8_t kIsimAid[7] = { 0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x04 };
    static const uint8_t kUsimAid[7] = { 0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02 };
    if (qmiUimOpenLogicalChannel(kIsimAid, sizeof(kIsimAid), -1, &ch, NULL, NULL, NULL, NULL) == 0 ||
        qmiUimOpenLogicalChannel(kUsimAid, sizeof(kUsimAid), -1, &ch, NULL, NULL, NULL, NULL) == 0) {
        ch_opened = 1;
    } else {
        RLOGE("requestSimAuthentication: QMI UIM OpenLogicalChannel failed");
        goto error;
    }

    uint8_t cla = (ch < 4) ? ch : (uint8_t)(0x40 | (ch - 4));
    uint8_t p2 = (authContext == AUTH_CONTEXT_EAP_AKA) ? 0x81 : 0x80;
    uint8_t apdu[260];
    apdu[0] = cla;
    apdu[1] = 0x88; /* AUTHENTICATE */
    apdu[2] = 0x00;
    apdu[3] = p2;
    apdu[4] = (uint8_t)binAuthLen;
    memcpy(apdu + 5, binAuthData, (size_t)binAuthLen);

    uint8_t resp[512];
    size_t resp_len = 0;
    if (qmiUimSendApdu(ch, apdu, (size_t)(5 + binAuthLen), resp, sizeof(resp), &resp_len) < 0 ||
        resp_len < 2) {
        RLOGE("requestSimAuthentication: QMI UIM SendAPDU(AUTHENTICATE) failed");
        goto close_channel;
    }

    /* Handle 61 xx (response data available -> GET RESPONSE) */
    if (resp[resp_len - 2] == 0x61) {
        uint8_t get_resp[5] = { cla, 0xC0, 0x00, 0x00, resp[resp_len - 1] };
        if (qmiUimSendApdu(ch, get_resp, sizeof(get_resp), resp, sizeof(resp), &resp_len) < 0 ||
            resp_len < 2) {
            RLOGE("requestSimAuthentication: QMI UIM GET RESPONSE failed");
            goto close_channel;
        }
    }

    response.sw1 = resp[resp_len - 2];
    response.sw2 = resp[resp_len - 1];
    size_t payload_len = resp_len - 2;
    if (payload_len > 0) {
        response.simResponse = (char *)calloc(2 * payload_len + 8, 1);
        if (!response.simResponse) goto close_channel;
        base64_encode(resp, response.simResponse, (int)payload_len);
    } else {
        response.simResponse = strdup("");
    }

    RLOGI("requestSimAuthentication(QMI UIM): ctx=%d sw=%02x%02x payload_len=%zu",
          authContext, response.sw1, response.sw2, payload_len);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    free(response.simResponse);
    if (ch_opened) qmiUimCloseLogicalChannel(ch);
    free(binAuthData);
    return;

close_channel:
    if (ch_opened) qmiUimCloseLogicalChannel(ch);
error:
    free(binAuthData);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestTransmitApduBasic( void *data, size_t datalen,
                                      RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, len;
    char *cmd = NULL;
    char *line = NULL;
    RIL_SIM_APDU *p_args = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_APDU *)data;

    if ((p_args->data == NULL) || (strlen(p_args->data) == 0)) {
        if (p_args->p3 < 0) {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x\"", 8, p_args->cla,
                    p_args->instruction, p_args->p1, p_args->p2);
        } else {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x\"", 10,
                    p_args->cla, p_args->instruction, p_args->p1, p_args->p2,
                    p_args->p3);
        }
    } else {
        asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x%s\"",
                10 + (int)strlen(p_args->data), p_args->cla,
                p_args->instruction, p_args->p1, p_args->p2, p_args->p3,
                p_args->data);
    }
    err = at_send_command_singleline(cmd, "+CSIM:", &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int getPDP() {
    int ret = -1;

    for (int i = 0; i < MAX_PDP; i++) {
        if (s_PDP[i].state == PDP_IDLE) {
            s_PDP[i].state = PDP_BUSY;
            ret = s_PDP[i].cid;
            break;
        }
    }
    return ret;
}

static void putPDP(int cid) {
    if (cid < 1 || cid > MAX_PDP ) {
        return;
    }

    s_PDP[cid - 1].state = PDP_IDLE;
}

static void requestSetupDataCall(void *data, size_t datalen, RIL_Token t)
{
    const char *apn = NULL;
    char *cmd = NULL;
    int err = -1;
    int cid = -1;
    ATResponse *p_response = NULL;

    apn = ((const char **)data)[2];

#ifdef USE_TI_COMMANDS
    // Config for multislot class 10 (probably default anyway eh?)
    err = at_send_command("AT%CPRIM=\"GMM\",\"CONFIG MULTISLOT_CLASS=<10>\"",
                        NULL);

    err = at_send_command("AT%DATA=2,\"UART\",1,,\"SER\",\"UART\",0", NULL);
#endif /* USE_TI_COMMANDS */

    int fd, qmistatus;
    size_t cur = 0;
    size_t len;
    ssize_t written, rlen;
    char status[32] = {0};
    int retry = 10;
    const char *pdp_type;

    RLOGD("requesting data connection to APN '%s'", apn);

    if (useDataconn()) {
        pdp_type = datalen > 6 * sizeof(char *) ? ((const char **)data)[6] : "IP";
        int cid = startDataconn(apn, pdp_type);
        if (cid < 0) {
            RIL_Data_Call_Response_v11 response = {
                .status = 0xffff /* PDP_FAIL_ERROR_UNSPECIFIED */,
                .suggestedRetryTime = -1,
                .cid = -1,
                .active = 0,
                .type = "", .ifname = "", .addresses = "", .dnses = "", .gateways = "",
                .pcscf = "", .mtu = 0,
            };
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
            return;
        }
        RIL_Data_Call_Response_v11 response;
        fillDataconnResponse(cid, &response);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
        return;
    }

    fd = open ("/dev/qmi", O_RDWR);
    if (fd >= 0) { /* the device doesn't exist on the emulator */

        RLOGD("opened the qmi device\n");
        asprintf(&cmd, "up:%s", apn);
        len = strlen(cmd);

        while (cur < len) {
            do {
                written = write (fd, cmd + cur, len - cur);
            } while (written < 0 && errno == EINTR);

            if (written < 0) {
                RLOGE("### ERROR writing to /dev/qmi");
                close(fd);
                goto error;
            }

            cur += written;
        }

        // wait for interface to come online

        do {
            sleep(1);
            do {
                rlen = read(fd, status, 31);
            } while (rlen < 0 && errno == EINTR);

            if (rlen < 0) {
                RLOGE("### ERROR reading from /dev/qmi");
                close(fd);
                goto error;
            } else {
                status[rlen] = '\0';
                RLOGD("### status: %s", status);
            }
        } while (strncmp(status, "STATE=up", 8) && strcmp(status, "online") && --retry);

        close(fd);

        if (retry == 0) {
            RLOGE("### Failed to get data connection up\n");
            goto error;
        }

        qmistatus = system("netcfg buried_eth0 dhcp");

        RLOGD("netcfg buried_eth0 dhcp: status %d\n", qmistatus);

        if (qmistatus < 0) goto error;

    } else {
        const char* radioInterfaceName = getRadioInterfaceName();
        if (setInterfaceState(radioInterfaceName, kInterfaceUp) != RIL_E_SUCCESS) {
            goto error;
        }

        if (datalen > 6 * sizeof(char *)) {
            pdp_type = ((const char **)data)[6];
        } else {
            pdp_type = "IP";
        }

        cid = getPDP();
        if (cid < 1) {
            RLOGE("SETUP_DATA_CALL MAX_PDP reached.");
            RIL_Data_Call_Response_v11 response;
            response.status = 0x41 /* PDP_FAIL_MAX_ACTIVE_PDP_CONTEXT_REACHED */;
            response.suggestedRetryTime = -1;
            response.cid = cid;
            response.active = -1;
            response.type = "";
            response.ifname = "";
            response.addresses = "";
            response.dnses = "";
            response.gateways = "";
            response.pcscf = "";
            response.mtu = 0;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(RIL_Data_Call_Response_v11));
            at_response_free(p_response);
            return;
        }

        asprintf(&cmd, "AT+CGDCONT=%d,\"%s\",\"%s\",,0,0", cid, pdp_type, apn);
        //FIXME check for error here
        err = at_send_command(cmd, NULL);
        free(cmd);

        // Set required QoS params to default
        err = at_send_command("AT+CGQREQ=1", NULL);

        // Set minimum QoS params to default
        err = at_send_command("AT+CGQMIN=1", NULL);

        // packet-domain event reporting
        err = at_send_command("AT+CGEREP=1,0", NULL);

        // Hangup anything that's happening there now
        err = at_send_command("AT+CGACT=1,0", NULL);

        // Start data on PDP context 1
        err = at_send_command("ATD*99***1#", &p_response);

        if (err < 0 || p_response->success == 0) {
            goto error;
        }
    }

    requestOrSendDataCallList(cid, &t);

    at_response_free(p_response);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDeactivateDataCall(void *data, RIL_Token t)
{
    const char *p_cid = ((const char **)data)[0];
    int cid = p_cid ? atoi(p_cid) : -1;
    RIL_Errno rilErrno = RIL_E_GENERIC_FAILURE;

    if (useDataconn()) {
        stopDataconn(cid);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        sendDataconnCallList(NULL);
        return;
    }
    if (cid < 1  || cid > MAX_PDP) {
        RIL_onRequestComplete(t, rilErrno, NULL, 0);
        return;
    }

    const char* radioInterfaceName = getRadioInterfaceName();
    rilErrno = setInterfaceState(radioInterfaceName, kInterfaceDown);
    RIL_onRequestComplete(t, rilErrno, NULL, 0);
    putPDP(cid);
    requestOrSendDataCallList(-1, NULL);
}

static void requestSMSAcknowledge(void *data, size_t datalen __unused, RIL_Token t)
{
    int ackSuccess;
    int err;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    ackSuccess = ((int *)data)[0];

    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
        if (err < 0) {
            goto error;
        }
    } else if (ackSuccess == 0)  {
        err = at_send_command("AT+CNMA=2", NULL);
        if (err < 0) {
            goto error;
        }
    } else {
        RLOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

void convertBytesToHex(uint8_t *bytes, int length, uint8_t *hex_str) {
    int i;
    unsigned char tmp;

    if (bytes == NULL || hex_str == NULL) {
        return;
    }
    for (i = 0; i < length; i++) {
        tmp = (unsigned char)((bytes[i] & 0xf0) >> 4);
        if (tmp <= 9) {
            *hex_str = (unsigned char)(tmp + '0');
        } else {
            *hex_str = (unsigned char)(tmp + 'A' - 10);
        }
        hex_str++;
        tmp = (unsigned char)(bytes[i] & 0x0f);
        if (tmp <= 9) {
            *hex_str = (unsigned char)(tmp + '0');
        } else {
            *hex_str = (unsigned char)(tmp + 'A' - 10);
        }
        hex_str++;
    }
}

#define TYPE_EF                                 4
#define RESPONSE_EF_SIZE                        15
#define TYPE_FILE_DES_LEN                       5
#define RESPONSE_DATA_FILE_DES_FLAG             2
#define RESPONSE_DATA_FILE_DES_LEN_FLAG         3
#define RESPONSE_DATA_FILE_TYPE                 6
#define RESPONSE_DATA_FILE_SIZE_1               2
#define RESPONSE_DATA_FILE_SIZE_2               3
#define RESPONSE_DATA_STRUCTURE                 13
#define RESPONSE_DATA_RECORD_LENGTH             14
#define RESPONSE_DATA_FILE_RECORD_LEN_1         6
#define RESPONSE_DATA_FILE_RECORD_LEN_2         7
#define EF_TYPE_TRANSPARENT                     0x01
#define EF_TYPE_LINEAR_FIXED                    0x02
#define EF_TYPE_CYCLIC                          0x06
#define USIM_DATA_OFFSET_2                      2
#define USIM_DATA_OFFSET_3                      3
#define USIM_FILE_DES_TAG                       0x82
#define USIM_FILE_SIZE_TAG                      0x80

bool convertUsimToSim(uint8_t *byteUSIM, int len, uint8_t *hexSIM) {
    int desIndex = -1;
    int sizeIndex = -1;
    uint8_t byteSIM[RESPONSE_EF_SIZE] = {0};

    if (byteUSIM == NULL || len < 4 || byteUSIM[0] != 0x62) {
        return false;
    }

    int fcpEnd = 2 + byteUSIM[1];
    if (fcpEnd > len) {
        RLOGW("convertUsimToSim: truncated FCP (have %d, expected %d)", len, fcpEnd);
        return false;
    }

    for (int i = 2; i + 2 <= fcpEnd;) {
        uint8_t tag = byteUSIM[i];
        uint8_t tlen = byteUSIM[i + 1];
        if (i + 2 + tlen > fcpEnd) {
            break;
        }
        if (tag == USIM_FILE_DES_TAG && tlen >= 1 && desIndex < 0) {
            desIndex = i;
        } else if (tag == USIM_FILE_SIZE_TAG && tlen >= 1 && sizeIndex < 0) {
            sizeIndex = i;
        }
        i += 2 + tlen;
    }

    if (desIndex < 0 || sizeIndex < 0) {
        RLOGW("convertUsimToSim: missing FCP TLV (desIndex=%d sizeIndex=%d len=%d)",
              desIndex, sizeIndex, len);
        return false;
    }

    uint8_t slen = byteUSIM[sizeIndex + 1];
    if (slen >= 2) {
        byteSIM[RESPONSE_DATA_FILE_SIZE_1] = byteUSIM[sizeIndex + 2];
        byteSIM[RESPONSE_DATA_FILE_SIZE_2] = byteUSIM[sizeIndex + 3];
    } else {
        byteSIM[RESPONSE_DATA_FILE_SIZE_1] = 0;
        byteSIM[RESPONSE_DATA_FILE_SIZE_2] = byteUSIM[sizeIndex + 2];
    }

    byteSIM[RESPONSE_DATA_FILE_TYPE] = TYPE_EF;
    uint8_t dlen = byteUSIM[desIndex + 1];
    uint8_t fdb = byteUSIM[desIndex + 2];
    if ((fdb & 0x07) == EF_TYPE_TRANSPARENT) {
        byteSIM[RESPONSE_DATA_STRUCTURE] = 0;
    } else if ((fdb & 0x07) == EF_TYPE_LINEAR_FIXED) {
        if (dlen < TYPE_FILE_DES_LEN) {
            return false;
        }
        byteSIM[RESPONSE_DATA_STRUCTURE] = 1;
        byteSIM[RESPONSE_DATA_RECORD_LENGTH] =
                (uint8_t)(((byteUSIM[desIndex + 4] & 0xff) << 8) +
                          (byteUSIM[desIndex + 5] & 0xff));
    } else if ((fdb & 0x07) == EF_TYPE_CYCLIC) {
        if (dlen < TYPE_FILE_DES_LEN) {
            return false;
        }
        byteSIM[RESPONSE_DATA_STRUCTURE] = 3;
        byteSIM[RESPONSE_DATA_RECORD_LENGTH] =
                (uint8_t)(((byteUSIM[desIndex + 4] & 0xff) << 8) +
                          (byteUSIM[desIndex + 5] & 0xff));
    }

    convertBytesToHex(byteSIM, RESPONSE_EF_SIZE, hexSIM);
    return true;
}

static int handleIsimSimIoViaQmi(const RIL_SIM_IO_v6 *p_args, RIL_Token t)
{
    static const uint8_t kIsimAid[7] = { 0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x04 };
    uint8_t ch = 0;
    if (qmiUimOpenLogicalChannel(kIsimAid, sizeof(kIsimAid), -1, &ch, NULL, NULL, NULL, NULL) < 0) {
        return -1;
    }

    uint8_t cla = (ch < 4) ? ch : (uint8_t)(0x40 | (ch - 4));
    uint8_t sel[7] = {
        cla, 0xA4, 0x00, 0x04, 0x02,
        (uint8_t)((p_args->fileid >> 8) & 0xff),
        (uint8_t)(p_args->fileid & 0xff),
    };
    uint8_t resp[512];
    size_t resp_len = 0;
    int rc = -1;

    if (qmiUimSendApdu(ch, sel, sizeof(sel), resp, sizeof(resp), &resp_len) < 0 || resp_len < 2) {
        goto done;
    }

    RIL_SIM_IO_Response sr;
    memset(&sr, 0, sizeof(sr));
    uint8_t hexSIM[RESPONSE_EF_SIZE * 2 + 1] = {0};
    char *hexResp = NULL;

    if (p_args->command == 192 /* GET RESPONSE */) {
        if (resp[resp_len - 2] == 0x61) {
            uint8_t get_resp[5] = { cla, 0xC0, 0x00, 0x00, resp[resp_len - 1] };
            if (qmiUimSendApdu(ch, get_resp, sizeof(get_resp),
                               resp, sizeof(resp), &resp_len) < 0 || resp_len < 2) {
                goto done;
            }
        }
        sr.sw1 = resp[resp_len - 2];
        sr.sw2 = resp[resp_len - 1];
        size_t plen = resp_len - 2;
        if (plen > 0 && resp[0] == 0x62 && convertUsimToSim(resp, (int)plen, hexSIM)) {
            sr.simResponse = (char *)hexSIM;
        } else if (plen > 0) {
            hexResp = (char *)calloc(plen * 2 + 1, 1);
            if (!hexResp) goto done;
            convertBytesToHexString((char *)resp, (int)plen, (unsigned char *)hexResp);
            sr.simResponse = hexResp;
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
        free(hexResp);
        rc = 0;
        goto done;
    } else if (p_args->command == 176 /* READ BINARY */ ||
               p_args->command == 178 /* READ RECORD */) {
        uint8_t cmd_apdu[5] = {
            cla,
            (uint8_t)(p_args->command & 0xff),
            (uint8_t)(p_args->p1 & 0xff),
            (uint8_t)(p_args->p2 & 0xff),
            (uint8_t)(p_args->p3 & 0xff),
        };
        if (qmiUimSendApdu(ch, cmd_apdu, sizeof(cmd_apdu),
                           resp, sizeof(resp), &resp_len) < 0 || resp_len < 2) {
            goto done;
        }
        sr.sw1 = resp[resp_len - 2];
        sr.sw2 = resp[resp_len - 1];
        size_t plen = resp_len - 2;
        hexResp = (char *)calloc(plen * 2 + 1, 1);
        if (!hexResp) goto done;
        if (plen > 0) {
            convertBytesToHexString((char *)resp, (int)plen, (unsigned char *)hexResp);
        }
        sr.simResponse = hexResp;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
        free(hexResp);
        rc = 0;
        goto done;
    }

done:
    qmiUimCloseLogicalChannel(ch);
    return rc;
}

static void  requestSIM_IO(void *data, size_t datalen __unused, RIL_Token t)
{
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO_v6 *p_args;
    char *line;

    /* For Convert USIM to SIM */
    uint8_t hexSIM[RESPONSE_EF_SIZE * 2 + sizeof(char)] = {0};

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO_v6 *)data;

    /* ISIM EFs cannot be read via default-app AT+CRSM; route through QMI UIM ISIM channel */
    int is_isim = (p_args->aidPtr != NULL && strncasecmp(p_args->aidPtr, "A0000000871004", 14) == 0) ||
                  (p_args->fileid == 0x6F02 || p_args->fileid == 0x6F03 ||
                   p_args->fileid == 0x6F04 || p_args->fileid == 0x6F07 ||
                   p_args->fileid == 0x6F09);
    if (is_isim && handleIsimSimIoViaQmi(p_args, t) == 0) {
        return;
    }

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        if (p_args->command == 192 /* GET RESPONSE */) {
            /*
             * Android IccFileHandler passes p3=15 (legacy 2G SIM size), which
             * causes USIM modems (e.g. SDX55) to truncate the 29+ byte FCP
             * template before the 0x80 file-size TLV. Omit p1/p2/p3 per 3GPP
             * TS 27.007 to request the full FCP.
             */
            asprintf(&cmd, "AT+CRSM=%d,%d", p_args->command, p_args->fileid);
        } else {
            asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
                        p_args->command, p_args->fileid,
                        p_args->p1, p_args->p2, p_args->p3);
        }
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);

    if ((err < 0 || p_response->success == 0) &&
        p_args->data == NULL && p_args->command == 192) {
        at_response_free(p_response);
        p_response = NULL;
        free(cmd);
        asprintf(&cmd, "AT+CRSM=%d,%d,0,0,0", p_args->command, p_args->fileid);
        err = at_send_command_singleline(cmd, "+CRSM:", &p_response);
    }

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = parseSimResponseLine(line, &sr);
    if (err < 0) {
        goto error;
    }
    if (sr.simResponse != NULL && sr.simResponse[0] != '\0' &&
        p_args->command == 192) {  // Get response
        size_t respHexLen = strlen(sr.simResponse);
        uint8_t *bytes = convertHexStringToBytes(sr.simResponse, respHexLen);
        if (bytes == NULL) {
            RLOGE("Failed to convert sim response to bytes");
            goto error;
        }
        size_t respBinLen = respHexLen / 2;
        if (respBinLen >= 2 && bytes[0] == 0x62 && respBinLen < (size_t)(2 + bytes[1])) {
            /* Modem truncated FCP; re-request with exact FCP length (2 + FCP len byte). */
            int fullLen = 2 + bytes[1];
            free(bytes);
            at_response_free(p_response);
            p_response = NULL;
            free(cmd);
            memset(&sr, 0, sizeof(sr));
            asprintf(&cmd, "AT+CRSM=192,%d,0,0,%d", p_args->fileid, fullLen);
            err = at_send_command_singleline(cmd, "+CRSM:", &p_response);
            if (err < 0 || p_response->success == 0) {
                goto error;
            }
            line = p_response->p_intermediates->line;
            err = parseSimResponseLine(line, &sr);
            if (err < 0 || sr.simResponse == NULL) {
                goto error;
            }
            respHexLen = strlen(sr.simResponse);
            respBinLen = respHexLen / 2;
            bytes = convertHexStringToBytes(sr.simResponse, respHexLen);
            if (bytes == NULL) {
                goto error;
            }
        }
        if (respBinLen < 2 || bytes[0] != 0x62) {
            RLOGE("Wrong FCP flag, unable to convert to sim ");
            free(bytes);
            goto error;
        }
        if (convertUsimToSim(bytes, (int)respBinLen, hexSIM)) {
          sr.simResponse = (char *)hexSIM;
        }
        free(bytes);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static int getSimlockRemainTimes(const char* type) {
    int err = -1;
    int remain_times = -1;
    char cmd[32] = {0};
    char *line = NULL;
    char *lock_type = NULL;
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CPINR=\"%s\"", type);
    err = at_send_command_multiline(cmd, "+CPINR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &lock_type);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &remain_times);
    if (err < 0) goto error;

error:
    at_response_free(p_response);
    return remain_times;
}

static void  requestEnterSimPin(int request, void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    int           remaintimes = -1;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if (datalen == sizeof(char*) || datalen == 2 * sizeof(char*)) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        if (request == RIL_REQUEST_ENTER_SIM_PIN) {
            remaintimes = getSimlockRemainTimes("SIM PIN");
        } else if (request == RIL_REQUEST_ENTER_SIM_PIN2) {
            remaintimes = getSimlockRemainTimes("SIM PIN2");
        }
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintimes,
            sizeof(remaintimes));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void  requestChangeSimPin(int request, void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse   *p_response = NULL;
    int           err;
    int           remaintimes = -1;
    char*         cmd = NULL;
    const char**  strings = (const char**)data;;

    if (datalen == 2 * sizeof(char*) || datalen == 3 * sizeof(char*)) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        if (request == RIL_REQUEST_CHANGE_SIM_PIN) {
            remaintimes = getSimlockRemainTimes("SIM PIN");
        } else if (request == RIL_REQUEST_ENTER_SIM_PUK) {
            remaintimes = getSimlockRemainTimes("SIM PUK");
        } else if (request == RIL_REQUEST_ENTER_SIM_PUK2) {
          remaintimes = getSimlockRemainTimes("SIM PUK2");
        }
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintimes,
            sizeof(remaintimes));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestChangeSimPin2(void *data, size_t datalen, RIL_Token t) {
    int err, ret;
    int remaintime = -1;
    char cmd[64] = {0};
    const char **strings = (const char **)data;
    ATResponse *p_response = NULL;

    if (datalen != 3 * sizeof(char *)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CPWD=\"P2\",\"%s\",\"%s\"", strings[0],
             strings[1]);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        remaintime = getSimlockRemainTimes("SIM PIN2");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintime,
                          sizeof(remaintime));
    at_response_free(p_response);
}

static void  requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    char cmd[128] = {0};
    const char *ussdRequest = (char *)(data);
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\"", ussdRequest);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestExitEmergencyMode(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    int err;
    ATResponse *p_response = NULL;

    err = at_send_command("AT+WSOS=0", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static uint64_t s_last_activity_info_query = 0;

static void requestGetActivityInfo(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    uint64_t curTime = ril_nano_time();
    RIL_ActivityStatsInfo stats =
    {
        0, // sleep_mode_time_ms
        ((curTime - s_last_activity_info_query) / 1000000) - 1, // idle_mode_time_ms
        {0, 0, 0, 0, 0}, // tx_mode_time_ms
        0 // rx_mode_time_ms
    };
    s_last_activity_info_query = curTime;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &stats, sizeof(stats));
}

// TODO: Use all radio types
static int techFromModemType(int mdmtype)
{
    int ret = -1;
    switch (mdmtype) {
        case MDM_CDMA:
            ret = RADIO_TECH_1xRTT;
            break;
        case MDM_EVDO:
            ret = RADIO_TECH_EVDO_A;
            break;
        case MDM_GSM:
            ret = RADIO_TECH_GPRS;
            break;
        case MDM_WCDMA:
            ret = RADIO_TECH_HSPA;
            break;
        case MDM_LTE:
            ret = RADIO_TECH_LTE;
        case MDM_NR:
            ret = RADIO_TECH_NR;
            break;
    }
    return ret;
}

static void requestGetCellInfoList(void *data __unused, size_t datalen __unused, RIL_Token t)
{
    uint64_t curTime = ril_nano_time();
    RIL_CellInfo_v12 ci[1] =
    {
        { // ci[0]
            1, // cellInfoType
            1, // registered
            RIL_TIMESTAMP_TYPE_MODEM,
            curTime - 1000, // Fake some time in the past
            { // union CellInfo
                {  // RIL_CellInfoGsm gsm
                    {  // gsm.cellIdneityGsm
                        s_mcc, // mcc
                        s_mnc, // mnc
                        s_lac, // lac
                        s_cid, // cid
                        0, //arfcn unknown
                        0x1, // Base Station Identity Code set to arbitrarily 1
                    },
                    {  // gsm.signalStrengthGsm
                        10, // signalStrength
                        0  // bitErrorRate
                        , INT_MAX // timingAdvance invalid value
                    }
                }
            }
        }
    };

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ci, sizeof(ci));
}

static void requestGetCellInfoList_1_6(void* data __unused, size_t datalen __unused, RIL_Token t) {
    RIL_CellInfo_v16 ci[1] = {{    // ci[0]
                               3,  // cellInfoType
                               1,  // registered
                               CELL_CONNECTION_PRIMARY_SERVING,
                               {        // union CellInfo
                                .lte = {// RIL_CellInfoLte_v12 lte
                                        {
                                                // RIL_CellIdentityLte_v12
                                                // lte.cellIdentityLte
                                                s_mcc,  // mcc
                                                s_mnc,  // mnc
                                                s_cid,  // ci
                                                0,      // pci
                                                s_lac,  // tac
                                                7,      // earfcn
                                        },
                                        {
                                                // RIL_LTE_SignalStrength_v8
                                                // lte.signalStrengthLte
                                                10,      // signalStrength
                                                44,      // rsrp
                                                3,       // rsrq
                                                30,      // rssnr
                                                0,       // cqi
                                                INT_MAX  // timingAdvance invalid value
                                        }}}}};

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ci, sizeof(ci));
}

static void requestSetCellInfoListRate(void *data, size_t datalen __unused, RIL_Token t)
{
    // For now we'll save the rate but no RIL_UNSOL_CELL_INFO_LIST messages
    // will be sent.
    assert (datalen == sizeof(int));
    s_cell_info_rate_ms = ((int *)data)[0];

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetHardwareConfig(void *data, size_t datalen, RIL_Token t)
{
   // TODO - hook this up with real query/info from radio.

   RIL_HardwareConfig hwCfg;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   hwCfg.type = -1;

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &hwCfg, sizeof(hwCfg));
}

static void requestGetTtyMode(void *data, size_t datalen, RIL_Token t)
{
   int  ttyModeResponse;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   ttyModeResponse = (getSIMStatus() == SIM_READY) ? 1  // TTY Full
                                                   : 0; // TTY Off

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &ttyModeResponse, sizeof(ttyModeResponse));
}

static void requestGetRadioCapability(void *data, size_t datalen, RIL_Token t)
{
   RIL_RadioCapability radioCapability;

   RIL_UNUSED_PARM(data);
   RIL_UNUSED_PARM(datalen);

   radioCapability.version = RIL_RADIO_CAPABILITY_VERSION;
   radioCapability.session = 0;
   radioCapability.phase   = 0;
   radioCapability.rat     = NR | LTE | WCDMA | GSM;
   strncpy(radioCapability.logicalModemUuid, "com.qualcomm.qti.ril.modem0", MAX_UUID_LENGTH);
   radioCapability.status  = RC_STATUS_SUCCESS;

   RIL_onRequestComplete(t, RIL_E_SUCCESS, &radioCapability, sizeof(radioCapability));
}

static void requestSetRadioCapability(void *data, size_t datalen, RIL_Token t)
{
  RIL_RadioCapability* rc = (RIL_RadioCapability*)data;
  RLOGV(
      "RadioCapability version %d session %d phase %d rat %d "
      "logicalModemUuid %s status %d",
      rc->version, rc->session, rc->phase, rc->rat, rc->logicalModemUuid,
      rc->status);
  // TODO(ender): do something about these numbers.
  RIL_onRequestComplete(t, RIL_E_SUCCESS, rc, datalen);
}

static void requestGetMute(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int muteResponse = 0;  // Mute disabled
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline("AT+CMUT?", "+CMUT:", &p_response);
    if (err < 0 || p_response->success) {
        goto done;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto done;

    at_tok_nextint(&line, &muteResponse);

done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &muteResponse, sizeof(muteResponse));
    at_response_free(p_response);
}

static void requestSetMute(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    char cmd[64] = {0};
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CMUT=%d", ((int *)data)[0]);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success) {
      RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
      RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestScreenState(void *data, RIL_Token t)
{
    int status = *((int *)data);

    if (!status) {
        /* Suspend */
        at_send_command("AT+CEREG=1", NULL);
        at_send_command("AT+CREG=1", NULL);
        at_send_command("AT+CGREG=1", NULL);
    } else {
       /* Resume */
        at_send_command("AT+CEREG=2", NULL);
        at_send_command("AT+CREG=2", NULL);
        at_send_command("AT+CGREG=2", NULL);
    }

   RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestQueryClip(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);
    RIL_UNUSED_PARM(data);

    int err = -1;
    int skip = 0;
    int response = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }

    err = at_send_command_singleline("AT+CLIP?", "+CLIP:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryClir(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);
    RIL_UNUSED_PARM(data);

    int err = -1;
    int response[2] = {1, 1};
    char *line = NULL;
    ATResponse *p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }

    err = at_send_command_singleline("AT+CLIR?", "+CLIR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetClir(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int n = ((int *)data)[0];
    char cmd[64] = {0};
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CLIR=%d", n);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static int forwardFromCCFCULine(char *line, RIL_CallForwardInfo *p_forward) {
    int err = -1;
    int i = 0;

    if (line == NULL || p_forward == NULL) {
      goto error;
    }

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->status));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->serviceClass));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        int numberType = 0;
        err = at_tok_nextint(&line, &numberType);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &p_forward->toa);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &(p_forward->number));

        /* tolerate null here */
        if (err < 0) return 0;

        if (at_tok_hasmore(&line)) {
            for (i = 0; i < 2; i++) {
                skipNextComma(&line);
            }

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &p_forward->timeSeconds);
                if (err < 0) {
                    p_forward->timeSeconds = 0;
                }
            }
        }
    }

    return 0;

error:
    return -1;
}

static void requestSetCallForward(RIL_CallForwardInfo *data,
                                  size_t datalen, RIL_Token t) {
    int err = -1;
    char cmd[128] = {0};
    size_t offset = 0;
    ATResponse *p_response = NULL;

    if (datalen != sizeof(*data) ||
            (data->status == 3 && data->number == NULL)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
                            data->reason,
                            data->status,
                            2,
                            data->toa,
                            data->number ? data->number : "",
                            data->serviceClass);
    offset += strlen(cmd);

    if (data->serviceClass == 0) {
        if (data->timeSeconds != 0 && data->status == 3) {
            snprintf(cmd + offset, sizeof(cmd) - offset, ",\"\",\"\",,%d",
                data->timeSeconds);
        }
    } else {
        if (data->timeSeconds != 0 && data->status == 3) {
            snprintf(cmd + offset, sizeof(cmd) - offset, ",\"\",\"\",,%d",
                data->timeSeconds);
        } else {
            strlcat(cmd, ",\"\"", sizeof(cmd) - offset);
        }
    }

    err = at_send_command_multiline(cmd, "+CCFCU:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryCallForward(RIL_CallForwardInfo *data,
                                    size_t datalen, RIL_Token t) {
    int err = -1;
    char cmd[128] = {0};
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;

    if (datalen != sizeof(*data)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCFCU=%d,2,%d,%d,\"%s\",%d", data->reason, 2,
            data->toa, data->number ? data->number : "", data->serviceClass);

    err = at_send_command_multiline(cmd, "+CCFCU:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_CallForwardInfo **forwardList = NULL, *forwardPool = NULL;
    int forwardCount = 0;
    int validCount = 0;
    int i = 0;

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next, forwardCount++) {
    }

    forwardList = (RIL_CallForwardInfo **)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo *));

    forwardPool = (RIL_CallForwardInfo *)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo));

    memset(forwardPool, 0, forwardCount * sizeof(RIL_CallForwardInfo));

    /* init the pointer array */
    for (i = 0; i < forwardCount; i++) {
        forwardList[i] = &(forwardPool[i]);
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        err = forwardFromCCFCULine(p_cur->line, forwardList[validCount]);
        forwardList[validCount]->reason = data->reason;
        if (err == 0) validCount++;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, validCount ? forwardList : NULL,
                          validCount * sizeof (RIL_CallForwardInfo *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryCallWaiting(void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = -1, mode = 0;
    int serviceClass = ((int *)data)[0];
    int response[2] = {0, 0};
    char cmd[32] = {0};
    char *line;
    ATLine *p_cur;
    ATResponse *p_response = NULL;

    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2");
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2,%d", serviceClass);
    }
    err = at_send_command_multiline(cmd, "+CCWA:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        line = p_cur->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &mode);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &serviceClass);
        if (err < 0) goto error;

        response[0] = mode;
        response[1] |= serviceClass;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetCallWaiting(void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    int err = -1;
    char cmd[32] = {0};
    int enable = ((int *)data)[0];
    int serviceClass = ((int *)data)[1];

    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d", enable);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d,%d", enable, serviceClass);
    }

    err = at_send_command(cmd,  &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestSetSuppServiceNotifications(void *data, size_t datalen,
                                               RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = 0;
    ATResponse *p_response = NULL;
    int mode = ((int *)data)[0];
    char cmd[32] = {0};

    snprintf(cmd, sizeof(cmd), "AT+CSSN=%d,%d", mode, mode);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestChangeBarringPassword(char **data, size_t datalen, RIL_Token t) {
    int err = -1;
    int result;
    char cmd[64] = {0};
    ATResponse *p_response = NULL;

    if (datalen != 3 * sizeof(char *) || data[0] == NULL || data[1] == NULL ||
        data[2] == NULL || strlen(data[0]) == 0 ||  strlen(data[1]) == 0 ||
        strlen(data[2]) == 0) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CPWD=\"%s\",\"%s\",\"%s\"", data[0], data[1],
             data[2]);

    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    at_response_free(p_response);
}

static void requestFacilityLock(int request, char **data,
                                size_t datalen, RIL_Token t) {
    int err = -1;
    int status = 0;
    int serviceClass = 0;
    int remainTimes = 10;
    char cmd[128] = {0};
    char *line = NULL;
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;
    RIL_Errno errnoType = RIL_E_GENERIC_FAILURE;

    char *type = data[0];

    if (datalen != 5 * sizeof(char *)) {
        goto error;
    }
    if (data[0] == NULL || data[1] == NULL ||
       (data[2] == NULL && request == RIL_REQUEST_SET_FACILITY_LOCK) ||
        strlen(data[0]) == 0 || strlen(data[1]) == 0 ||
       (request == RIL_REQUEST_SET_FACILITY_LOCK && strlen(data[2]) == 0 )) {
        errnoType = RIL_E_INVALID_ARGUMENTS;
        RLOGE("FacilityLock invalid arguments");
        goto error;
    }

    serviceClass = atoi(data[3]);
    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CLCK=\"%s\",%c,\"%s\"", data[0], *data[1],
                 data[2]);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CLCK=\"%s\",%c,\"%s\",%s", data[0],
                 *data[1], data[2], data[3]);
    }

    if (*data[1] == '2') {  // query status
        err = at_send_command_multiline(cmd, "+CLCK: ", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }
        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &status);
        if (err < 0) goto error;

        RIL_onRequestComplete(t, RIL_E_SUCCESS, &status, sizeof(int));
        at_response_free(p_response);
        return;
    } else {  // unlock/lock this facility
        err = at_send_command(cmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            errnoType = RIL_E_PASSWORD_INCORRECT;
            goto error;
        }
        errnoType = RIL_E_SUCCESS;
    }

error:
    if (!strcmp(data[0], "SC")) {
        remainTimes = getSimlockRemainTimes("SIM PIN");
    } else if (!strcmp(data[0], "FD")) {
        remainTimes = getSimlockRemainTimes("SIM PIN2");
    } else {
        remainTimes = 1;
    }

    RIL_onRequestComplete(t, errnoType, &remainTimes, sizeof(remainTimes));
    at_response_free(p_response);
}

static void requestSetSmscAddress(void *data, size_t datalen, RIL_Token t)
{
    ATResponse   *p_response = NULL;
    char          cmd[64] = {0};
    int           err = -1;

    if (getSIMStatus() != SIM_READY) {
         RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
         return;
    }

    if (data == NULL || strlen(data) == 0) {
        RLOGE("SET_SMSC_ADDRESS invalid address: %s", (char *)data);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CSCA=%s,%d", (char *)data, (int)datalen);

    err = at_send_command_singleline(cmd, "+CSCA:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestGetSmscAddress(void *data, size_t datalen, RIL_Token t)
{
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    ATResponse   *p_response = NULL;
    int           err = -1;
    char         *decidata = NULL;

    err = at_send_command_singleline( "AT+CSCA?", "+CSCA:", &p_response);
    if (err < 0 || p_response->success == 0) {
       goto error;
    }

    char *line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &decidata);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, decidata, strlen(decidata) + 1);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void setGsmBroadcastConfigData(int from, int to, int id, int outStrSize, char *outStr) {
    if (from < 0 || from > 0xffff || to < 0 || to > 0xffff) {
        RLOGE("setGsmBroadcastConfig data is invalid, [%d, %d]", from, to);
        return;
    }

    if (id != 0) {
        strlcat(outStr, ",", outStrSize);
    }

    int len = strlen(outStr);
    if (from == to) {
        snprintf(outStr + len, outStrSize - len, "%d", from);
    } else {
        snprintf(outStr + len, outStrSize - len, "%d-%d", from, to);
    }
}

static void requestSetSmsBroadcastConfig(void *data, size_t datalen,
                                         RIL_Token t) {
    int i = 0;
    int count = datalen / sizeof(RIL_GSM_BroadcastSmsConfigInfo *);
    int size = count * 16;
    char cmd[256] = {0};
    char *channel = (char *)alloca(size);
    char *languageId = (char *)alloca(size);
    ATResponse *p_response = NULL;
    RIL_GSM_BroadcastSmsConfigInfo **pGsmBci =
            (RIL_GSM_BroadcastSmsConfigInfo **)data;
    RIL_GSM_BroadcastSmsConfigInfo gsmBci = {0};

    memset(channel, 0, size);
    memset(languageId, 0, size);
    RLOGD("requestSetGsmBroadcastConfig %zu, count %d", datalen, count);

    for (i = 0; i < count; i++) {
        gsmBci = *(pGsmBci[i]);
        setGsmBroadcastConfigData(gsmBci.fromServiceId, gsmBci.toServiceId, i,
                                  size, channel);
        setGsmBroadcastConfigData(gsmBci.fromCodeScheme, gsmBci.toCodeScheme, i,
                                  size, languageId);
    }

    snprintf(cmd, sizeof(cmd), "AT+CSCB=%d,\"%s\",\"%s\"",
            (*pGsmBci[0]).selected ? 0 : 1, channel, languageId);
    int err = at_send_command_singleline( cmd, "+CSCB:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestGetSmsBroadcastConfig(void *data, size_t datalen,
                                         RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    int err = -1, mode, commas = 0, i = 0;
    char *line = NULL;
    char *serviceIds = NULL, *codeSchemes = NULL, *p = NULL;
    char *serviceId = NULL, *codeScheme = NULL;

    err = at_send_command_singleline("AT+CSCB?", "+CSCB:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &serviceIds);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &codeSchemes);
    if (err < 0) goto error;

    for (p = serviceIds; *p != '\0'; p++) {
        if (*p == ',') {
            commas++;
        }
    }
    RIL_GSM_BroadcastSmsConfigInfo **pGsmBci =
        (RIL_GSM_BroadcastSmsConfigInfo **)alloca((commas + 1) *
            sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
    memset(pGsmBci, 0, (commas + 1) * sizeof(RIL_GSM_BroadcastSmsConfigInfo *));

    for (i = 0; i < commas + 1; i++) {
        pGsmBci[i] = (RIL_GSM_BroadcastSmsConfigInfo *)alloca(
                sizeof(RIL_GSM_BroadcastSmsConfigInfo));
        memset(pGsmBci[i], 0, sizeof(RIL_GSM_BroadcastSmsConfigInfo));

        err = at_tok_nextstr(&serviceIds, &serviceId);
        if (err < 0) goto error;
        pGsmBci[i]->toServiceId = pGsmBci[i]->fromServiceId = 0;
        if (strstr(serviceId, "-")) {
            sscanf(serviceId,"%d-%d", &pGsmBci[i]->fromServiceId,
                   &pGsmBci[i]->toServiceId);
        }

        err = at_tok_nextstr(&codeSchemes, &codeScheme);
        if (err < 0) goto error;
        pGsmBci[i]->toCodeScheme = pGsmBci[i]->fromCodeScheme = 0;
        if (strstr(codeScheme, "-")) {
            sscanf(codeScheme, "%d-%d", &pGsmBci[i]->fromCodeScheme,
                   &pGsmBci[i]->toCodeScheme);
        }

        pGsmBci[i]->selected = (mode == 0 ? false : true);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, pGsmBci,
        (commas + 1) * sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * <AcT>: integer type; access technology selected
 * 0 GSM
 * 1 GSM Compact
 * 2 UTRAN
 * 3 GSM w/EGPRS (see NOTE 1)
 * 4 UTRAN w/HSDPA (see NOTE 2)
 * 5 UTRAN w/HSUPA (see NOTE 2)
 * 6 UTRAN w/HSDPA and HSUPA (see NOTE 2)
 * 7 E-UTRAN
 * 8 EC-GSM-IoT (A/Gb mode) (see NOTE 3)
 * 9 E-UTRAN (NB-S1 mode) (see NOTE 4)
 * 10  E-UTRA connected to a 5GCN (see NOTE 5)
 * 11  NR connected to a 5GCN (see NOTE 5)
 * 12  NG-RAN
 * 13  E-UTRA-NR dual connectivity (see NOTE 6)
 */
int mapRadioAccessNetworkToTech(RIL_RadioAccessNetworks network) {
    switch (network) {
        case GERAN:  // GSM EDGE
            return 3;
        case UTRAN:
            return 6;
        case EUTRAN:
            return 7;
        case NGRAN:
            return 11;
        default:
            return 7;  // LTE
    }
}

static void requestSetNetworlSelectionManual(void *data, RIL_Token t) {
    int err = -1;
    char cmd[64] = {0};
    ATResponse *p_response = NULL;
    RIL_NetworkOperator *operator = (RIL_NetworkOperator *)data;

    if (operator->act != UNKNOWN) {
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,2,\"%s\"", operator->operatorNumeric);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,2,\"%s\",%d",
            operator->operatorNumeric, operator->act);
    }
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
        !strcmp(p_response->finalResponse, "+CME ERROR: 30")) {
          RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestStkServiceIsRunning(RIL_Token t)
{
    int err = -1;
    ATResponse *p_response = NULL;

    s_stkServiceRunning = true;
    if (NULL != s_stkUnsolResponse) {
       RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND,
                            s_stkUnsolResponse, strlen(s_stkUnsolResponse) + 1);
       free(s_stkUnsolResponse);
       s_stkUnsolResponse = NULL;
       RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
       return;
    }

    err = at_send_command_singleline("AT+CUSATD?", "+CUSATD:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestStkSendEnvelope(void *data, RIL_Token t)
{
    int ret = -1, err = -1;
    char cmd[128] = {0};
    ATResponse *p_response = NULL;

    if (data == NULL || strlen((char *)data) == 0) {
        RLOGE("STK sendEnvelope data is invalid");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CUSATE=\"%s\"", (char *)data);
    err = at_send_command_singleline(cmd, "+CUSATE:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

        // type of alpha data is 85, such as 850C546F6F6C6B6974204D656E75
        char *p = strstr(p_response->p_intermediates->line, "85");
        if (p != NULL) {
            char alphaStrHexLen[3] = {0};
            char alphaStr[1024] = {0};
            uint8_t *alphaBytes = NULL;
            int len = 0;

            p = p + strlen("85");
            strncpy(alphaStrHexLen, p, 2);
            len = strtoul(alphaStrHexLen, NULL, 16);
            strncpy(alphaStr, p + 2, len * 2);
            alphaBytes = convertHexStringToBytes(alphaStr, strlen(alphaStr));
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_CC_ALPHA_NOTIFY, alphaBytes,
                                      strlen((char *)alphaBytes));
            free(alphaBytes);
        }
    }
    at_response_free(p_response);
}

static void requestStksendTerminalResponse(void *data, RIL_Token t)
{
    int ret = -1, err = -1;
    char cmd[128] = {0};
    ATResponse *p_response = NULL;

    if (data == NULL || strlen((char *)data) == 0) {
        RLOGE("STK sendTerminalResponse data is invalid");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CUSATT=\"%s\"", (char *)data);
    err = at_send_command_singleline( cmd, "+CUSATT:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestEccDial(void *data, RIL_Token t) {
    char cmd[64] = {0};
    const char *clir = NULL;
    int err = -1;
    RIL_EmergencyDial *p_eccDial = (RIL_EmergencyDial *)data;

    switch (p_eccDial->dialInfo.clir) {
        case 0:  /* subscription default */
            clir = "";
            break;
        case 1:  /* invocation */
            clir = "I";
            break;
        case 2:  /* suppression */
            clir = "i";
            break;
        default:
            break;
    }

    if (p_eccDial->routing == ROUTING_MERGENCY ||
        p_eccDial->routing ==  ROUTING_UNKNOWN) {
      if (p_eccDial->categories == CATEGORY_UNSPECIFIED) {
          snprintf(cmd, sizeof(cmd), "ATD%s@,#%s;", p_eccDial->dialInfo.address, clir);
      } else {
          snprintf(cmd, sizeof(cmd), "ATD%s@%d,#%s;", p_eccDial->dialInfo.address,
              p_eccDial->categories, clir);
      }
    } else {  // ROUTING_NORMAL
        snprintf(cmd, sizeof(cmd), "ATD%s%s;", p_eccDial->dialInfo.address, clir);
    }

    err = at_send_command(cmd, NULL);
    if (err != 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestStartKeepalive(void* data, size_t datalen __unused, RIL_Token t) {
    RIL_KeepaliveRequest* kaRequest = (RIL_KeepaliveRequest*)data;
    if (kaRequest->cid > MAX_PDP) {
        RLOGE("Invalid cid for keepalive!");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    RIL_KeepaliveStatus resp;
    resp.sessionHandle = __sync_fetch_and_add(&s_session_handle, 1);
    resp.code = KEEPALIVE_ACTIVE;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &resp, sizeof(resp));
}

void getConfigSlotStatus(RIL_SimSlotStatus_V1_2 *pSimSlotStatusList, int count) {
    if (pSimSlotStatusList == NULL || count <= 0) {
        return;
    }

    QmiUimSlotInfo qmiSlots[SIM_COUNT];
    int qmiCount = qmiUimGetSlotsStatus(qmiSlots, SIM_COUNT);

    for (int i = 0; i < count; i++) {
        RIL_SimSlotStatus_V1_2 *st = &pSimSlotStatusList[i];
        if (qmiCount > i) {
            st->base.cardState = qmiSlots[i].card_present ? RIL_CARDSTATE_PRESENT
                                                          : RIL_CARDSTATE_ABSENT;
            st->base.slotState = qmiSlots[i].slot_active ? SLOT_STATE_ACTIVE
                                                         : SLOT_STATE_INACTIVE;
            st->base.logicalSlotId = (uint32_t)qmiSlots[i].logical_slot;
            if (st->base.cardState != RIL_CARDSTATE_ABSENT) {
                st->base.atr = "";
                st->base.iccid = (char *)calloc(64, sizeof(char));
                if (st->base.iccid != NULL) {
                    if (qmiSlots[i].iccid[0] != '\0') {
                        snprintf(st->base.iccid, 64, "%s", qmiSlots[i].iccid);
                    } else if (i == 0) {
                        getIccId(st->base.iccid, 64);
                    }
                }
            }
        } else if (i == 0) {
            st->base.cardState = (getSIMStatus() == SIM_ABSENT) ? RIL_CARDSTATE_ABSENT
                                                                : RIL_CARDSTATE_PRESENT;
            st->base.slotState = SLOT_STATE_ACTIVE;
            st->base.logicalSlotId = 0;
            if (st->base.cardState != RIL_CARDSTATE_ABSENT) {
                st->base.atr = "";
                st->base.iccid = (char *)calloc(64, sizeof(char));
                if (st->base.iccid != NULL) {
                    getIccId(st->base.iccid, 64);
                }
            }
        } else {
            st->base.cardState = RIL_CARDSTATE_ABSENT;
            st->base.slotState = SLOT_STATE_ACTIVE;
            st->base.logicalSlotId = (uint32_t)i;
        }
        st->eid = "";
    }
}

void sendUnsolNetworkScanResult(void *param __unused) {
    RIL_NetworkScanResult scanr;
    memset(&scanr, 0, sizeof(scanr));
    scanr.status = COMPLETE;
    scanr.error = RIL_E_SUCCESS;
    scanr.network_infos = NULL;
    scanr.network_infos_length = 0;
    RIL_onUnsolicitedResponse(RIL_UNSOL_NETWORK_SCAN_RESULT, &scanr, sizeof(scanr));
}

void onIccSlotStatus(RIL_Token t) {
    RIL_SimSlotStatus_V1_2 *pSimSlotStatusList =
        (RIL_SimSlotStatus_V1_2 *)calloc(SIM_COUNT, sizeof(RIL_SimSlotStatus_V1_2));

    getConfigSlotStatus(pSimSlotStatusList, SIM_COUNT);

    if (t == NULL) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_CONFIG_ICC_SLOT_STATUS, pSimSlotStatusList,
                 SIM_COUNT * sizeof(RIL_SimSlotStatus_V1_2));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, pSimSlotStatusList,
                SIM_COUNT * sizeof(RIL_SimSlotStatus_V1_2));
    }

    if (pSimSlotStatusList != NULL) {
        for (int i = 0; i < SIM_COUNT; i++) {
            free(pSimSlotStatusList[i].base.iccid);
        }
        free(pSimSlotStatusList);
    }
}

#if defined(ANDROID_MULTI_SIM)
/*
 * Secondary SIM slot (slot2) handler for DSDS.
 *
 * SDX55 exposes a single MHI AT channel (/dev/wwan0at0), owned by slot1.
 * Never issue AT commands or mutate slot1 state from here: answer identity
 * (IMEI 2, IMEI SV, baseband) via QMI DMS, physical card presence via QMI
 * UIM, and radio/network queries cleanly so Android exposes full dual-SIM
 * metadata without interfering with slot1 5G/LTE/IMS.
 */
static void
onRequestSlot2(int request, void *data, size_t datalen, RIL_Token t)
{
    RLOGD("onRequestSlot2: %s, sStateSlot2: %d", requestToString(request), sStateSlot2);

    if (sStateSlot2 == RADIO_STATE_UNAVAILABLE) {
        switch (request) {
            case RIL_REQUEST_GET_SIM_STATUS:
            case RIL_REQUEST_DEVICE_IDENTITY:
            case RIL_REQUEST_GET_IMEI:
            case RIL_REQUEST_GET_IMEISV:
            case RIL_REQUEST_BASEBAND_VERSION:
            case RIL_REQUEST_GET_RADIO_CAPABILITY:
            case RIL_REQUEST_GET_MODEM_STACK_STATUS:
            case RIL_REQUEST_ENABLE_MODEM:
            case RIL_REQUEST_ALLOW_DATA:
            case RIL_REQUEST_CONFIG_GET_SLOT_STATUS:
            case RIL_REQUEST_CONFIG_SET_SLOT_MAPPING:
            case RIL_REQUEST_CONFIG_GET_PHONE_CAPABILITY:
            case RIL_REQUEST_CONFIG_SET_MODEM_CONFIG:
            case RIL_REQUEST_CONFIG_GET_MODEM_CONFIG:
            case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM:
                break;
            default:
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                return;
        }
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus_v1_5 *p_card_status =
                    (RIL_CardStatus_v1_5 *)calloc(1, sizeof(RIL_CardStatus_v1_5));
            if (p_card_status == NULL) {
                RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
                break;
            }
            p_card_status->base.base.base.card_state = RIL_CARDSTATE_ABSENT;
            p_card_status->base.base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
            p_card_status->base.base.base.gsm_umts_subscription_app_index = -1;
            p_card_status->base.base.base.cdma_subscription_app_index = -1;
            p_card_status->base.base.base.ims_subscription_app_index = -1;
            p_card_status->base.base.base.num_applications = 0;
            p_card_status->base.base.physicalSlotId = 1;
            p_card_status->base.base.atr = NULL;
            p_card_status->base.base.iccid = NULL;
            p_card_status->base.eid = "";

            QmiUimSlotInfo qmiSlots[SIM_COUNT];
            int qmiCount = qmiUimGetSlotsStatus(qmiSlots, SIM_COUNT);
            if (qmiCount >= 2 && qmiSlots[1].card_present) {
                p_card_status->base.base.base.card_state = RIL_CARDSTATE_PRESENT;
                if (qmiSlots[1].iccid[0] != '\0') {
                    p_card_status->base.base.iccid = strdup(qmiSlots[1].iccid);
                }
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS, p_card_status, sizeof(*p_card_status));
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_DEVICE_IDENTITY:
            requestDeviceIdentityForSlot(1, t);
            break;
        case RIL_REQUEST_GET_IMEI: {
            char imei[32] = {0};
            if (qmiDmsGetSerialNumbersForSlot(1, imei, sizeof(imei),
                                              NULL, 0, NULL, 0, NULL, 0) == 0) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, imei, strlen(imei) + 1);
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_GET_IMEISV: {
            char imei[32] = {0};
            char imeisv[16] = {0};
            if (qmiDmsGetSerialNumbersForSlot(1, imei, sizeof(imei),
                                              imeisv, sizeof(imeisv),
                                              NULL, 0, NULL, 0) == 0 &&
                imeisv[0] != '\0') {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, imeisv, strlen(imeisv) + 1);
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_BASEBAND_VERSION: {
            char ver[256] = {0};
            if (qmiDmsGetBasebandVersion(ver, sizeof(ver)) == 0) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, ver, strlen(ver) + 1);
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_RADIO_POWER: {
            int onOff = (data != NULL && datalen >= sizeof(int)) ? ((int *)data)[0] : 0;
            RIL_RadioState next = onOff ? RADIO_STATE_ON : RADIO_STATE_OFF;
            RIL_RadioState prev;
            pthread_mutex_lock(&s_state_mutex);
            prev = sStateSlot2;
            sStateSlot2 = next;
            pthread_mutex_unlock(&s_state_mutex);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            if (prev != next) {
                RIL_onUnsolicitedResponseForSlot(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                                 NULL, 0, RIL_SOCKET_2);
            }
            break;
        }
        case RIL_REQUEST_SHUTDOWN: {
            RIL_RadioState prev;
            pthread_mutex_lock(&s_state_mutex);
            prev = sStateSlot2;
            sStateSlot2 = RADIO_STATE_OFF;
            pthread_mutex_unlock(&s_state_mutex);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            if (prev != RADIO_STATE_OFF) {
                RIL_onUnsolicitedResponseForSlot(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                                 NULL, 0, RIL_SOCKET_2);
            }
            break;
        }
        case RIL_REQUEST_GET_RADIO_CAPABILITY: {
            RIL_RadioCapability rc;
            memset(&rc, 0, sizeof(rc));
            rc.version = RIL_RADIO_CAPABILITY_VERSION;
            rc.session = 0;
            rc.phase = 0;
            rc.rat = NR | LTE | WCDMA | GSM;
            strncpy(rc.logicalModemUuid, "com.qualcomm.qti.ril.modem1", MAX_UUID_LENGTH);
            rc.status = RC_STATUS_SUCCESS;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &rc, sizeof(rc));
            break;
        }
        case RIL_REQUEST_SET_RADIO_CAPABILITY:
            requestSetRadioCapability(data, datalen, t);
            break;
        case RIL_REQUEST_VOICE_REGISTRATION_STATE: {
            char *resp[REG_STATE_LEN];
            memset(resp, 0, sizeof(resp));
            resp[0] = (char *)"0";
            RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
            break;
        }
        case RIL_REQUEST_DATA_REGISTRATION_STATE: {
            char *resp[REG_DATA_STATE_LEN];
            memset(resp, 0, sizeof(resp));
            resp[0] = (char *)"0";
            RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
            break;
        }
        case RIL_REQUEST_OPERATOR: {
            char *resp[3] = { NULL, NULL, NULL };
            RIL_onRequestComplete(t, RIL_E_SUCCESS, resp, sizeof(resp));
            break;
        }
        case RIL_REQUEST_SIGNAL_STRENGTH: {
            RIL_SignalStrength_v12 ss;
            initEmptySignalStrength(&ss);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &ss, sizeof(ss));
            break;
        }
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE: {
            int mode = 0;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &mode, sizeof(mode));
            break;
        }
        case RIL_REQUEST_DATA_CALL_LIST:
        case RIL_REQUEST_GET_CURRENT_CALLS:
        case RIL_REQUEST_GET_CELL_INFO_LIST:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_slot2PreferredNetworkType,
                                  sizeof(s_slot2PreferredNetworkType));
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            if (data != NULL && datalen >= sizeof(int)) {
                s_slot2PreferredNetworkType = *(int *)data;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP:
        case RIL_REQUEST_GET_ALLOWED_NETWORK_TYPES_BITMAP:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_slot2AllowedNetworkTypes,
                                  sizeof(s_slot2AllowedNetworkTypes));
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE_BITMAP:
        case RIL_REQUEST_SET_ALLOWED_NETWORK_TYPES_BITMAP:
            if (data != NULL && datalen >= sizeof(int)) {
                s_slot2AllowedNetworkTypes = *(int *)data;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_ACTIVITY_INFO:
            requestGetActivityInfo(data, datalen, t);
            break;
        case RIL_REQUEST_GET_HARDWARE_CONFIG:
            requestGetHardwareConfig(data, datalen, t);
            break;
        case RIL_REQUEST_GET_CARRIER_RESTRICTIONS:
            requestGetCarrierRestrictions(data, datalen, t);
            break;
        case RIL_REQUEST_GET_MUTE: {
            int mute = 0;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &mute, sizeof(mute));
            break;
        }
        case RIL_REQUEST_QUERY_TTY_MODE: {
            int tty = 0;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &tty, sizeof(tty));
            break;
        }
        case RIL_REQUEST_VOICE_RADIO_TECH: {
            int tech = RADIO_TECH_UNKNOWN;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &tech, sizeof(tech));
            break;
        }
        case RIL_REQUEST_ARE_UICC_APPLICATIONS_ENABLED: {
            bool enabled = true;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &enabled, sizeof(enabled));
            break;
        }
        case RIL_REQUEST_IS_NR_DUAL_CONNECTIVITY_ENABLED:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &isNrDualConnectivityEnabled,
                                  sizeof(isNrDualConnectivityEnabled));
            break;
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE: {
            int size = 5;
            int response[20] = {0};
            for (int i = 1; i <= size; i++) {
                response[i] = i - 1;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (size + 1) * sizeof(int));
            break;
        }
        case RIL_REQUEST_GET_MODEM_STACK_STATUS:
        case RIL_REQUEST_ENABLE_MODEM:
        case RIL_REQUEST_ALLOW_DATA:
        case RIL_REQUEST_SET_DATA_PROFILE:
        case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
        case RIL_REQUEST_SCREEN_STATE:
        case RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER:
        case RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA:
        case RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA:
        case RIL_REQUEST_SET_SYSTEM_SELECTION_CHANNELS:
        case RIL_REQUEST_SET_LOCATION_UPDATES:
        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
        case RIL_REQUEST_SET_TTY_MODE:
        case RIL_REQUEST_SET_MUTE:
        case RIL_REQUEST_SET_SIM_CARD_POWER:
        case RIL_REQUEST_ENABLE_UICC_APPLICATIONS:
        case RIL_REQUEST_ENABLE_NR_DUAL_CONNECTIVITY:
        case RIL_REQUEST_SET_DATA_THROTTLING:
        case RIL_REQUEST_SET_CARRIER_RESTRICTIONS:
        case RIL_REQUEST_SET_BAND_MODE:
        case RIL_REQUEST_GET_BARRING_INFO:
        case RIL_REQUEST_GET_SLICING_CONFIG:
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
        case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFIG_GET_SLOT_STATUS:
            RIL_requestTimedCallback(onIccSlotStatus, (void *)t, NULL);
            break;
        case RIL_REQUEST_CONFIG_SET_SLOT_MAPPING:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFIG_GET_PHONE_CAPABILITY: {
            RIL_PhoneCapability phoneCapability;
            memset(&phoneCapability, 0, sizeof(phoneCapability));
            phoneCapability.maxActiveData = 1;
            phoneCapability.maxActiveInternetData = 1;
            phoneCapability.isInternetLingeringSupported = false;
            for (int num = 0; num < SIM_COUNT; num++) {
                phoneCapability.logicalModemList[num].modemId = num;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &phoneCapability, sizeof(phoneCapability));
            break;
        }
        case RIL_REQUEST_CONFIG_GET_MODEM_CONFIG: {
            RIL_ModemConfig mdConfig;
            mdConfig.numOfLiveModems = SIM_COUNT;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &mdConfig, sizeof(RIL_ModemConfig));
            break;
        }
        case RIL_REQUEST_CONFIG_SET_MODEM_CONFIG: {
            RIL_ModemConfig *mdConfig = (RIL_ModemConfig *)data;
            if (mdConfig == NULL || mdConfig->numOfLiveModems < 1 ||
                mdConfig->numOfLiveModems > SIM_COUNT) {
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM: {
            int *modemId = (int *)data;
            if (modemId == NULL || *modemId < 0 || *modemId >= SIM_COUNT) {
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        default:
            RLOGD("onRequestSlot2: Request %s not supported", requestToString(request));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}
#endif

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Because onRequest function could be called from multiple different thread,
 * we must ensure that the underlying at_send_command_* function
 * is atomic.
 */
#if defined(ANDROID_MULTI_SIM)
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t,
           RIL_SOCKET_ID socket_id)
{
    if (socket_id != RIL_SOCKET_1) {
        onRequestSlot2(request, data, datalen, t);
        return;
    }
#else
static void
onRequest (int request, void *data, size_t datalen, RIL_Token t)
{
#endif
    ATResponse *p_response;
    int err;

    RLOGD("onRequest: %s, sState: %d", requestToString(request), sState);

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS and ALLOW_DATA
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
        && request != RIL_REQUEST_ALLOW_DATA
    ) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS)
     */
    if (sState == RADIO_STATE_OFF) {
        switch(request) {
            case RIL_REQUEST_ALLOW_DATA:
            case RIL_REQUEST_ARE_UICC_APPLICATIONS_ENABLED:
            case RIL_REQUEST_BASEBAND_VERSION:
            case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
            case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            case RIL_REQUEST_CDMA_SUBSCRIPTION:
            case RIL_REQUEST_DEVICE_IDENTITY:
            case RIL_REQUEST_ENABLE_UICC_APPLICATIONS:
            case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            case RIL_REQUEST_GET_ACTIVITY_INFO:
            case RIL_REQUEST_GET_CARRIER_RESTRICTIONS:
            case RIL_REQUEST_GET_CURRENT_CALLS:
            case RIL_REQUEST_GET_IMEI:
            case RIL_REQUEST_GET_MUTE:
            case RIL_REQUEST_SET_MUTE:
            case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
            case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_GET_RADIO_CAPABILITY:
            case RIL_REQUEST_GET_SIM_STATUS:
            case RIL_REQUEST_NV_RESET_CONFIG:
            case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
            case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            case RIL_REQUEST_QUERY_TTY_MODE:
            case RIL_REQUEST_RADIO_POWER:
            case RIL_REQUEST_SET_BAND_MODE:
            case RIL_REQUEST_SET_CARRIER_RESTRICTIONS:
            case RIL_REQUEST_SET_LOCATION_UPDATES:
            case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            case RIL_REQUEST_SET_SIM_CARD_POWER:
            case RIL_REQUEST_SET_TTY_MODE:
            case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            case RIL_REQUEST_STOP_LCE:
            case RIL_REQUEST_VOICE_RADIO_TECH:
            case RIL_REQUEST_SCREEN_STATE:
                // Process all the above, even though the radio is off
                break;

            default:
                // For all others, say NOT_AVAILABLE because the radio is off
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                return;
        }
    }

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: {
            RIL_CardStatus_v1_5 *p_card_status;
            char *p_buffer;
            int buffer_size;

            int result = getCardStatus(&p_card_status);
            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_GET_CURRENT_CALLS:
            requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_DIAL:
            requestDial(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP:
            requestHangup(data, datalen, t);
            break;
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        case RIL_REQUEST_CONFERENCE:
        case RIL_REQUEST_UDUB:
             requestCallSelection(data, datalen, t, request);
             break;
        case RIL_REQUEST_ANSWER:
            at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
            s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

            if (getSIMStatus() != SIM_READY) {
                RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
            } else {
                // Success or failure is ignored by the upper layer here.
                // It will call GET_CURRENT_CALLS and determine success that way.
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_SEPARATE_CONNECTION:
            {
                char  cmd[12];
                int   party = ((int*)data)[0];

                if (getSIMStatus() == SIM_ABSENT) {
                    RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                    return;
                }
                // Make sure that party is in a valid range.
                // (Note: The Telephony middle layer imposes a range of 1 to 7.
                // It's sufficient for us to just make sure it's single digit.)
                if (party > 0 && party < 10) {
                    sprintf(cmd, "AT+CHLD=2%d", party);
                    at_send_command(cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            break;

        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_DTMF: {
            char c = ((char *)data)[0];
            char *cmd;
            asprintf(&cmd, "AT+VTS=%c", (int)c);
            at_send_command(cmd, NULL);
            free(cmd);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_SEND_SMS:
        case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
            requestSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_SEND_SMS:
            requestCdmaSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_IMS_SEND_SMS:
            requestImsSendSMS(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_OPEN_CHANNEL:
            requestSimOpenChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_CLOSE_CHANNEL:
            requestSimCloseChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
            requestSimTransmitApduChannel(data, datalen, t);
            break;
        case RIL_REQUEST_SIM_AUTHENTICATION: {
            RIL_SimAuthentication *sim_auth = (RIL_SimAuthentication *)data;
            if ((sim_auth->authContext == AUTH_CONTEXT_EAP_SIM ||
                 sim_auth->authContext == AUTH_CONTEXT_EAP_AKA) &&
                sim_auth->authData != NULL) {
                requestSimAuthentication(sim_auth->authContext, sim_auth->authData, t);
            } else {
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
            requestTransmitApduBasic(data, datalen, t);
            break;
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDataCall(data, datalen, t);
            break;
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            requestDeactivateDataCall(data, t);
            break;
        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;

        case RIL_REQUEST_GET_IMSI:
            p_response = NULL;
            // The SIM stays busy for a few seconds after it reports READY
            for (int tries = 0; tries < 10; tries++) {
                at_response_free(p_response);
                p_response = NULL;
                err = at_send_command_numeric("AT+CIMI", &p_response);
                if (err == 0 && p_response->success) break;
                usleep(500 * 1000);
            }

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_GET_IMEI:
            p_response = NULL;
            err = at_send_command_numeric("AT+CGSN", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;


        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data,datalen,t);
            break;

        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;

        case RIL_REQUEST_CANCEL_USSD:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                break;
            }
            p_response = NULL;
            err = at_send_command_numeric("AT+CUSD=2", &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    p_response->p_intermediates->line, sizeof(char *));
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
                break;
            }
            p_response = NULL;
            err = at_send_command("AT+COPS=0", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;

        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            requestSetNetworlSelectionManual(data, t);
            break;

        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;

        case RIL_REQUEST_OEM_HOOK_RAW:
            // echo back data
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;


        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            int i;
            const char ** cur;

            RLOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);


            for (i = (datalen / sizeof (char *)), cur = (const char **)data ;
                    i > 0 ; cur++, i --) {
                RLOGD("> '%s'", *cur);
            }

            // echo back strings
            RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
            break;
        }

        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;

        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            char * cmd;
            p_response = NULL;
            asprintf(&cmd, "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }

        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PIN2:
            requestEnterSimPin(request, data, datalen, t);
            break;

        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PUK2:
        case RIL_REQUEST_CHANGE_SIM_PIN:
            requestChangeSimPin(request, data, datalen, t);
            break;

        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestChangeSimPin2(data, datalen, t);
            break;

        case RIL_REQUEST_IMS_REGISTRATION_STATE: {
            int reply[2];
            //0==unregistered, 1==registered
            reply[0] = s_ims_registered;

            //to be used when changed to include service supporated info
            //reply[1] = s_ims_services;

            // FORMAT_3GPP(1) vs FORMAT_3GPP2(2);
            reply[1] = s_ims_format;

            RLOGD("IMS_REGISTRATION=%d, format=%d ",
                    reply[0], reply[1]);
            if (reply[1] != -1) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, reply, sizeof(reply));
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }

        case RIL_REQUEST_VOICE_RADIO_TECH:
            {
                int tech = techFromModemType(TECH(sMdmInfo));
                if (tech < 0 )
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                else
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &tech, sizeof(tech));
            }
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            requestSetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            requestGetPreferredNetworkType(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_CELL_INFO_LIST:
            requestGetCellInfoList(data, datalen, t);
            break;

        case RIL_REQUEST_GET_CELL_INFO_LIST_1_6:
            requestGetCellInfoList_1_6(data, datalen, t);
            break;

        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            requestSetCellInfoListRate(data, datalen, t);
            break;

        case RIL_REQUEST_GET_HARDWARE_CONFIG:
            requestGetHardwareConfig(data, datalen, t);
            break;

        case RIL_REQUEST_SHUTDOWN:
            requestShutdown(t);
            break;

        case RIL_REQUEST_QUERY_TTY_MODE:
            requestGetTtyMode(data, datalen, t);
            break;

        case RIL_REQUEST_GET_RADIO_CAPABILITY:
            requestGetRadioCapability(data, datalen, t);
            break;

        case RIL_REQUEST_SET_RADIO_CAPABILITY:
            requestSetRadioCapability(data, datalen, t);
            break;

        case RIL_REQUEST_GET_MUTE:
            requestGetMute(data, datalen, t);
            break;

        case RIL_REQUEST_SET_MUTE:
            requestSetMute(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE: {
          int size = 5;
          int response[20] = {0};
          for (int i = 1; i <= size; i++) {
            response[i] = i - 1;
          }
          RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (size + 1) * sizeof(int));
          break;
        }

        case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
        case RIL_REQUEST_ALLOW_DATA:
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        case RIL_REQUEST_SET_BAND_MODE:
        case RIL_REQUEST_SET_CARRIER_RESTRICTIONS:
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        case RIL_REQUEST_SET_LOCATION_UPDATES:
        case RIL_REQUEST_SET_TTY_MODE:
        case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_NV_RESET_CONFIG:
            requestNvResetConfig(data, datalen, t);
            break;

        case RIL_REQUEST_BASEBAND_VERSION:
            requestCdmaBaseBandVersion(request, data, datalen, t);
            break;

        case RIL_REQUEST_DEVICE_IDENTITY:
            requestDeviceIdentity(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_SUBSCRIPTION:
            requestCdmaSubscription(request, data, datalen, t);
            break;

        case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE:
            requestCdmaGetSubscriptionSource(request, data, datalen, t);
            break;

        case RIL_REQUEST_START_LCE:
        case RIL_REQUEST_STOP_LCE:
        case RIL_REQUEST_PULL_LCEDATA:
            if (getSIMStatus() == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_LCE_NOT_SUPPORTED, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaGetRoamingPreference(request, data, datalen, t);
            } else {
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaSetSubscriptionSource(request, data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestCdmaSetRoamingPreference(request, data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
            if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
                requestExitEmergencyMode(data, datalen, t);
            } else {
                // VTS tests expect us to silently do nothing
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;

        case RIL_REQUEST_GET_ACTIVITY_INFO:
            requestGetActivityInfo(data, datalen, t);
            break;

        case RIL_REQUEST_SCREEN_STATE:
            requestScreenState(data, t);
            break;

        case RIL_REQUEST_SET_DATA_PROFILE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;

        case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
            requestQueryCallForward(data, datalen, t);
            break;

        case RIL_REQUEST_SET_CALL_FORWARD:
            requestSetCallForward(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_CLIP:
            requestQueryClip(data, datalen, t);
            break;

        case RIL_REQUEST_GET_CLIR:
            requestQueryClir(data, datalen, t);
            break;

        case RIL_REQUEST_SET_CLIR:
            requestSetClir(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_CALL_WAITING:
            requestQueryCallWaiting(data, datalen, t);
            break;

        case RIL_REQUEST_SET_CALL_WAITING:
            requestSetCallWaiting(data, datalen, t);
            break;

        case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
            requestSetSuppServiceNotifications(data, datalen, t);
            break;

        case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
            requestChangeBarringPassword(data, datalen, t);
            break;

        case RIL_REQUEST_QUERY_FACILITY_LOCK: {
            char *lockData[4];
            lockData[0] = ((char **)data)[0];
            lockData[1] = "2";
            lockData[2] = ((char **)data)[1];
            lockData[3] = ((char **)data)[2];
            requestFacilityLock(request, lockData, datalen + sizeof(char *), t);
            break;
        }

        case RIL_REQUEST_SET_FACILITY_LOCK:
            requestFacilityLock(request, data, datalen, t);
            break;

        case RIL_REQUEST_GET_SMSC_ADDRESS:
            requestGetSmscAddress(data, datalen, t);
            break;

        case RIL_REQUEST_SET_SMSC_ADDRESS:
            requestSetSmscAddress(data, datalen, t);
            break;

        case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
            requestSetSmsBroadcastConfig(data, datalen, t);
            break;

        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
            requestGetSmsBroadcastConfig(data, datalen, t);
            break;

        case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
            requestStkServiceIsRunning(t);
            break;

        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
            requestStkSendEnvelope(data, t);
            break;

        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
            requestStksendTerminalResponse(data, t);
            break;

        // New requests after P.
        case RIL_REQUEST_START_NETWORK_SCAN:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            // send unsol network scan results after a short while
            RIL_requestTimedCallback (sendUnsolNetworkScanResult, NULL, &TIMEVAL_SIMPOLL);
            break;
        case RIL_REQUEST_GET_MODEM_STACK_STATUS:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ENABLE_MODEM:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_EMERGENCY_DIAL:
            requestEccDial(data, t);
            break;
        case RIL_REQUEST_SET_SIM_CARD_POWER:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE_BITMAP:
            requestSetPreferredNetworkType(request, data, datalen, t);
            break;
        case RIL_REQUEST_SET_ALLOWED_NETWORK_TYPES_BITMAP:
            requestSetPreferredNetworkType(request, data, datalen, t);
            break;
        case RIL_REQUEST_GET_ALLOWED_NETWORK_TYPES_BITMAP:
            requestGetPreferredNetworkType(request, data, datalen, t);
            break;
        case RIL_REQUEST_ENABLE_NR_DUAL_CONNECTIVITY:
            if (data == NULL || datalen != sizeof(int)) {
                RIL_onRequestComplete(t, RIL_E_INTERNAL_ERR, NULL, 0);
                break;
            }
            int nrDualConnectivityState = *(int *)(data);
            isNrDualConnectivityEnabled = (nrDualConnectivityState == 1) ? true : false;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_IS_NR_DUAL_CONNECTIVITY_ENABLED:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &isNrDualConnectivityEnabled,
                    sizeof(isNrDualConnectivityEnabled));
            break;
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP:
            requestGetPreferredNetworkType(request, data, datalen, t);
            break;
        case RIL_REQUEST_SET_SYSTEM_SELECTION_CHANNELS:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_SLICING_CONFIG:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_CARRIER_RESTRICTIONS:
            requestGetCarrierRestrictions(data, datalen, t);
            break;

        // Radio config requests
        case RIL_REQUEST_CONFIG_GET_SLOT_STATUS:
            RIL_requestTimedCallback(onIccSlotStatus, (void *)t, NULL);
            break;
        case RIL_REQUEST_CONFIG_SET_SLOT_MAPPING:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_CONFIG_GET_PHONE_CAPABILITY: {
            RIL_PhoneCapability *phoneCapability =
                    (RIL_PhoneCapability *)alloca(sizeof(RIL_PhoneCapability));
            memset(phoneCapability, 0, sizeof(RIL_PhoneCapability));
            phoneCapability->maxActiveData = 1;
            // DSDS is 1, and DSDA is 2, now only support DSDS
            phoneCapability->maxActiveInternetData = 1;
            // DSDA can support internet lingering
            phoneCapability->isInternetLingeringSupported = false;
            for (int num = 0; num < SIM_COUNT; num++) {
                phoneCapability->logicalModemList[num].modemId = num;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS,
                            phoneCapability, sizeof(RIL_PhoneCapability));
            break;
        }
        case RIL_REQUEST_CONFIG_SET_MODEM_CONFIG: {
            RIL_ModemConfig *mdConfig = (RIL_ModemConfig*)(data);
            if (mdConfig == NULL || mdConfig->numOfLiveModems < 1 ||
                mdConfig->numOfLiveModems > SIM_COUNT) {
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_CONFIG_GET_MODEM_CONFIG: {
            RIL_ModemConfig mdConfig;
            mdConfig.numOfLiveModems = SIM_COUNT;

            RIL_onRequestComplete(t, RIL_E_SUCCESS, &mdConfig, sizeof(RIL_ModemConfig));
            break;
        }
        case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM: {
            int *modemId = (int*)(data);
            if (modemId == NULL || *modemId < 0 || *modemId >= SIM_COUNT) {
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_ENABLE_UICC_APPLICATIONS: {
            if (data == NULL || datalen != sizeof(int)) {
                RIL_onRequestComplete(t, RIL_E_INTERNAL_ERR, NULL, 0);
                break;
            }
            bool newEnabled = (*(int *)(data) != 0);
            bool changed = (newEnabled != areUiccApplicationsEnabled);
            areUiccApplicationsEnabled = newEnabled;
            RLOGI("ENABLE_UICC_APPLICATIONS: enable=%d (changed=%d, sState=%d)",
                  areUiccApplicationsEnabled ? 1 : 0, changed ? 1 : 0, sState);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

            if (!areUiccApplicationsEnabled) {
                if (useDataconn()) {
                    stopDataconn(0);
                    sendDataconnCallList(NULL);
                }
                s_lastRegTech = RADIO_TECH_UNKNOWN;
                s_lastRegAct = -1;
                s_lastSignalValid = 0;
                if (sState == RADIO_STATE_ON) {
                    at_send_command("AT+COPS=2", NULL);
                    s_copsDeregistered = 1;
                }
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
            } else {
                s_lastSignalValid = 0;
                if (sState == RADIO_STATE_ON) {
                    enableNetworkRegistrationUrks();
                    s_copsDeregistered = 0;
                    at_send_command("AT+COPS=0", NULL);
                    RIL_requestTimedCallback(notifyNetworkAndSignal, NULL, &TIMEVAL_RAT_SETTLE);
                    static const struct timeval settle_3s = {3, 500000};
                    RIL_requestTimedCallback(notifyNetworkAndSignal, NULL, &settle_3s);
                }
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_ARE_UICC_APPLICATIONS_ENABLED:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &areUiccApplicationsEnabled,
                    sizeof(areUiccApplicationsEnabled));
            break;
        case RIL_REQUEST_CDMA_SEND_SMS_EXPECT_MORE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_GET_BARRING_INFO:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SET_DATA_THROTTLING:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_START_KEEPALIVE:
            requestStartKeepalive(data, datalen, t);
            break;
        case RIL_REQUEST_STOP_KEEPALIVE:
            if (data == NULL || datalen != sizeof(int)) {
                RIL_onRequestComplete(t, RIL_E_INTERNAL_ERR, NULL, 0);
                break;
            }
            int sessionHandle = *(int*)(data);
            if ((int32_t)sessionHandle < s_session_handle) {
                // check that the session handle is one we've assigned previously
                // note that this doesn't handle duplicate stop requests properly
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            } else {
                RLOGE("Invalid session handle for keepalive!");
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            }
            break;
        case RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        default:
            RLOGD("Request not supported. Tech: %d",TECH(sMdmInfo));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
#if defined(ANDROID_MULTI_SIM)
static RIL_RadioState
currentState(RIL_SOCKET_ID socket_id)
{
    if (socket_id != RIL_SOCKET_1) {
        return sStateSlot2;
    }
    return sState;
}
#else
static RIL_RadioState
currentState()
{
    return sState;
}
#endif
/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */

static int
onSupports (int requestCode __unused)
{
    //@@@ todo

    return 1;
}

static void onCancel (RIL_Token t __unused)
{
    //@@@todo

}

static const char * getVersion(void)
{
    return "android reference-ril 1.0";
}

static void
setRadioTechnology(ModemInfo *mdm, int newtech)
{
    RLOGD("setRadioTechnology(%d)", newtech);

    int oldtech = TECH(mdm);

    if (newtech != oldtech) {
        RLOGD("Tech change (%d => %d)", oldtech, newtech);
        TECH(mdm) = newtech;
        if (techFromModemType(newtech) != techFromModemType(oldtech)) {
            int tech = techFromModemType(TECH(sMdmInfo));
            if (tech > 0 ) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                                          &tech, sizeof(tech));
            }
        }
    }
}

static void
setRadioState(RIL_RadioState newState)
{
    RLOGD("setRadioState(%d)", newState);
    RIL_RadioState oldState;
    int slot2Changed = 0;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

    if (s_closed > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || s_closed > 0) {
        sState = newState;

        pthread_cond_broadcast (&s_state_cond);
    }

#if defined(ANDROID_MULTI_SIM)
    if (newState == RADIO_STATE_UNAVAILABLE && sStateSlot2 != RADIO_STATE_UNAVAILABLE) {
        sStateSlot2 = RADIO_STATE_UNAVAILABLE;
        slot2Changed = 1;
    } else if (sStateSlot2 == RADIO_STATE_UNAVAILABLE && newState != RADIO_STATE_UNAVAILABLE) {
        sStateSlot2 = newState;
        slot2Changed = 1;
    }
#endif

    pthread_mutex_unlock(&s_state_mutex);


    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0);
        // Sim state can change as result of radio state change
        RIL_onUnsolicitedResponse (RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                    NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread
         */
        if (sState == RADIO_STATE_ON) {
            onRadioPowerOn();
        }
    }
#if defined(ANDROID_MULTI_SIM)
    if (slot2Changed) {
        RIL_onUnsolicitedResponseForSlot(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                         NULL, 0, RIL_SOCKET_2);
        RIL_onUnsolicitedResponseForSlot(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                         NULL, 0, RIL_SOCKET_2);
    }
#else
    (void)slot2Changed;
#endif
}

/** Returns RUIM_NOT_READY on error */
static SIM_Status
getRUIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (sState == RADIO_STATE_OFF || sState == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = SIM_READY;

done:
    at_response_free(p_response);
    return ret;
}

/** Returns SIM_NOT_READY on error */
static SIM_Status
getSIMStatus()
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    RLOGD("getSIMStatus(). sState: %d",sState);
    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start (&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp (cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp (cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp (cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp (cpinResult, "READY"))  {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = (sState == RADIO_STATE_ON) ? SIM_READY : SIM_NOT_READY;

done:
    at_response_free(p_response);
    return ret;
}

static void getIccId(char *iccid, int size) {
    int err = 0;
    ATResponse *p_response = NULL;

    if (iccid == NULL) {
        RLOGE("iccid buffer is null");
        return;
    }
    err = at_send_command_numeric("AT+CICCID", &p_response);
    if (err == 0 && p_response->success != 0) {
        snprintf(iccid, size, "%s", p_response->p_intermediates->line);
        goto error;
    }
    at_response_free(p_response);
    p_response = NULL;

    // Real modems don't know AT+CICCID, so read EF_ICCID (2FE2) instead.
    // Its 10 bytes hold the digits as swapped BCD nibbles, padded with F.
    err = at_send_command_singleline("AT+CRSM=176,12258,0,0,10", "+CRSM:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    {
        char *line = p_response->p_intermediates->line;
        int sw1, sw2, n = 0;
        char *hex = NULL;
        if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &sw1) < 0 ||
            at_tok_nextint(&line, &sw2) < 0 || at_tok_nextstr(&line, &hex) < 0 ||
            sw1 != 144 || hex == NULL) {
            goto error;
        }
        for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0' && n < size - 1; i += 2) {
            char lo = hex[i + 1], hi = hex[i];
            if (lo != 'F' && lo != 'f' && n < size - 1) iccid[n++] = lo;
            if (hi != 'F' && hi != 'f' && n < size - 1) iccid[n++] = hi;
        }
        iccid[n] = '\0';
    }

error:
    at_response_free(p_response);
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus_v1_5 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_USIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_USIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_USIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_ABSENT = 6
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_NOT_READY = 7
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_READY = 8
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_PIN = 9
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_PUK = 10
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // RUIM_NETWORK_PERSONALIZATION = 11
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
           NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_ABSENT = 12
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_NOT_READY = 13
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          "A0000000871004", "ISIM", 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_READY = 14
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          "A0000000871004", "ISIM", 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_PIN = 15
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_PUK = 16
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // ISIM_NETWORK_PERSONALIZATION = 17
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },

    };
    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else if (!areUiccApplicationsEnabled) {
        /* IRadioSim: keep card PRESENT + ICCID accessible, expose 0 active apps */
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 3;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v1_5 *p_card_status = calloc(1, sizeof(RIL_CardStatus_v1_5));
    p_card_status->base.base.base.card_state = card_state;
    p_card_status->base.base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->base.base.base.gsm_umts_subscription_app_index = -1;
    p_card_status->base.base.base.cdma_subscription_app_index = -1;
    p_card_status->base.base.base.ims_subscription_app_index = -1;
    p_card_status->base.base.base.num_applications = num_apps;
    p_card_status->base.base.physicalSlotId = 0;
    p_card_status->base.base.atr = NULL;
    p_card_status->base.base.iccid = NULL;
    p_card_status->base.eid = "";
    if (sim_status != SIM_ABSENT) {
        p_card_status->base.base.iccid = (char *)calloc(64, sizeof(char));
        getIccId(p_card_status->base.base.iccid, 64);
    }

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->base.base.base.applications[i] = app_status_array[SIM_ABSENT];
    }
    RLOGD("enter getCardStatus module, num_apps= %d",num_apps);
    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        p_card_status->base.base.base.num_applications = 3;
        p_card_status->base.base.base.gsm_umts_subscription_app_index = 0;
        p_card_status->base.base.base.cdma_subscription_app_index = 1;
        p_card_status->base.base.base.ims_subscription_app_index = 2;

        // Get the correct app status
        p_card_status->base.base.base.applications[0] = app_status_array[sim_status];
        p_card_status->base.base.base.applications[1] = app_status_array[sim_status + RUIM_ABSENT];
        p_card_status->base.base.base.applications[2] = app_status_array[sim_status + ISIM_ABSENT];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v1_5 *p_card_status) {
    if (p_card_status == NULL) {
        return;
    }
    free(p_card_status->base.base.iccid);
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */

static void pollSIMState (void *param __unused)
{
    ATResponse *p_response;
    int ret;

    // The SIM answers "SIM busy" for a while after AT+CFUN=1, so keep polling once on
    if (sState == RADIO_STATE_OFF) {
        return;
    }

    switch(getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            RLOGI("SIM ABSENT or LOCKED");
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            RLOGI("SIM_READY");
            onSIMReady();
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;
    }
}

/** returns 1 if on, 0 if off, and -1 on error */
static int isRadioOn()
{
    ATResponse *p_response = NULL;
    int err;
    char *line;
    char ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err < 0 || p_response->success == 0) {
        // assume radio is off
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) goto error;

    at_response_free(p_response);

    return (int)ret;

error:

    at_response_free(p_response);
    return -1;
}

/**
 * Parse the response generated by a +CTEC AT command
 * The values read from the response are stored in current and preferred.
 * Both current and preferred may be null. The corresponding value is ignored in that case.
 *
 * @return: -1 if some error occurs (or if the modem doesn't understand the +CTEC command)
 *          1 if the response includes the current technology only
 *          0 if the response includes both current technology and preferred mode
 */
int parse_technology_response( const char *response, int *current, int32_t *preferred )
{
    int err;
    char *line, *p;
    int ct;
    int32_t pt = 0;
    char *str_pt;

    line = p = strdup(response);
    RLOGD("Response: %s", line);
    err = at_tok_start(&p);
    if (err || !at_tok_hasmore(&p)) {
        RLOGD("err: %d. p: %s", err, p);
        free(line);
        return -1;
    }

    err = at_tok_nextint(&p, &ct);
    if (err) {
        free(line);
        return -1;
    }
    if (current) *current = ct;

    RLOGD("line remaining after int: %s", p);

    err = at_tok_nexthexint(&p, &pt);
    if (err) {
        free(line);
        return 1;
    }
    if (preferred) {
        *preferred = pt;
    }
    free(line);

    return 0;
}

int query_supported_techs( ModemInfo *mdm __unused, int *supported )
{
    ATResponse *p_response;
    int err, val, techs = 0;
    char *tok;
    char *line;

    RLOGD("query_supported_techs");
    err = at_send_command_singleline("AT+CTEC=?", "+CTEC:", &p_response);
    if (err || !p_response->success)
        goto error;
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err || !at_tok_hasmore(&line))
        goto error;
    while (!at_tok_nextint(&line, &val)) {
        techs |= ( 1 << val );
    }
    if (supported) *supported = techs;
    return 0;
error:
    at_response_free(p_response);
    return -1;
}

/**
 * query_ctec. Send the +CTEC AT command to the modem to query the current
 * and preferred modes. It leaves values in the addresses pointed to by
 * current and preferred. If any of those pointers are NULL, the corresponding value
 * is ignored, but the return value will still reflect if retrieving and parsing of the
 * values succeeded.
 *
 * @mdm Currently unused
 * @current A pointer to store the current mode returned by the modem. May be null.
 * @preferred A pointer to store the preferred mode returned by the modem. May be null.
 * @return -1 on error (or failure to parse)
 *         1 if only the current mode was returned by modem (or failed to parse preferred)
 *         0 if both current and preferred were returned correctly
 */
int query_ctec(ModemInfo *mdm __unused, int *current, int32_t *preferred)
{
    ATResponse *response = NULL;
    int err;
    int res;

    RLOGD("query_ctec. current: %p, preferred: %p", current, preferred);
    err = at_send_command_singleline("AT+CTEC?", "+CTEC:", &response);
    if (!err && response->success) {
        res = parse_technology_response(response->p_intermediates->line, current, preferred);
        at_response_free(response);
        return res;
    }
    RLOGE("Error executing command: %d. response: %p. status: %d", err, response, response? response->success : -1);
    at_response_free(response);
    return -1;
}

int is_multimode_modem(ModemInfo *mdm)
{
    ATResponse *response;
    int err;
    char *line;
    int tech;
    int32_t preferred;

    if (query_ctec(mdm, &tech, &preferred) == 0) {
        mdm->currentTech = tech;
        mdm->preferredNetworkMode = preferred;
        if (query_supported_techs(mdm, &mdm->supportedTechs)) {
            return 0;
        }
        return 1;
    }
    return 0;
}

/**
 * Find out if our modem is GSM, CDMA or both (Multimode)
 */
static void probeForModemMode(ModemInfo *info)
{
    ATResponse *response;
    int err;
    assert (info);
    // Currently, our only known multimode modem is qemu's android modem,
    // which implements the AT+CTEC command to query and set mode.
    // Try that first

    if (is_multimode_modem(info)) {
        RLOGI("Found Multimode Modem. Supported techs mask: %8.8x. Current tech: %d",
            info->supportedTechs, info->currentTech);
        return;
    }

    /* Being here means that our modem is not multimode */
    info->isMultimode = 0;

    /* CDMA Modems implement the AT+WNAM command */
    err = at_send_command_singleline("AT+WNAM","+WNAM:", &response);
    if (!err && response->success) {
        at_response_free(response);
        // TODO: find out if we really support EvDo
        info->supportedTechs = MDM_CDMA | MDM_EVDO;
        info->currentTech = MDM_CDMA;
        RLOGI("Found CDMA Modem");
        return;
    }
    if (!err) at_response_free(response);
    // TODO: find out if modem really supports WCDMA/LTE
    info->supportedTechs = MDM_GSM | MDM_WCDMA | MDM_LTE;
    info->currentTech = MDM_GSM;
    RLOGI("Found GSM Modem");
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void *param __unused)
{
    /*
     * Keep sState UNAVAILABLE until AT handshake and URC setup complete so
     * com.android.phone does not race RADIO_POWER (AT+CFUN=1) with init.
     */
    at_handshake();

    probeForModemMode(sMdmInfo);
    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network, GPRS, EPS (LTE), and 5GS (NR SA) registration events */
    enableNetworkRegistrationUrks();

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

#ifdef USE_TI_COMMANDS

    at_send_command("AT%CPI=3", NULL);

    /*  TI specific -- notifications when SMS is ready (currently ignored) */
    at_send_command("AT%CSTAT=1", NULL);

#endif /* USE_TI_COMMANDS */


    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState (RADIO_STATE_ON);
    } else {
        setRadioState (RADIO_STATE_OFF);
    }

    if (!s_signalPollStarted) {
        s_signalPollStarted = 1;
        RIL_requestTimedCallback(pollSignalStrength, NULL, &TIMEVAL_SIGNALPOLL);
    }
}

static void waitForClose()
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

static void sendUnsolImsNetworkStateChanged()
{
#if 0 // to be used when unsol is changed to return data.
    int reply[2];
    reply[0] = s_ims_registered;
    reply[1] = s_ims_services;
    reply[1] = s_ims_format;
#endif
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED,
            NULL, 0);
}

static int parseProactiveCmdInd(char *response) {
    int typePos = 0;
    int cmdType = 0;
    char tempStr[3] = {0};
    char *end = NULL;
    StkUnsolEvent ret = STK_UNSOL_EVENT_UNKNOWN;

    if (response == NULL || strlen(response) < 3) {
      return ret;
    }

    if (response[2] <= '7') {
       typePos = 10;
    } else {
       typePos = 12;
    }

    if ((int)strlen(response) < typePos + 1) {
      return ret;
    }

    memcpy(tempStr, &(response[typePos]), 2);
    cmdType = strtoul(tempStr, &end, 16);
    cmdType = 0xFF & cmdType;
    RLOGD("cmdType: %d",cmdType);

    switch (cmdType) {
       case STK_RUN_AT:
       case STK_SEND_DTMF:
       case STK_SEND_SMS:
       case STK_SEND_SS:
       case STK_SEND_USSD:
       case STK_PLAY_TONE:
       case STK_CLOSE_CHANNEL:
           ret = STK_UNSOL_EVENT_NOTIFY;
           break;
       case STK_REFRESH:
           if (strncasecmp(&(response[typePos + 2]), "04", 2) == 0) {  // SIM_RESET
               RLOGD("Type of Refresh is SIM_RESET");
               s_stkServiceRunning = false;
               ret = STK_UNSOL_PROACTIVE_CMD;
           } else {
               ret = STK_UNSOL_EVENT_NOTIFY;
           }
           break;
       default:
           ret = STK_UNSOL_PROACTIVE_CMD;
           break;
    }

    if (getSIMStatus() == SIM_ABSENT && s_stkServiceRunning) {
        s_stkServiceRunning = false;
    }

    if (false == s_stkServiceRunning) {
        ret = STK_UNSOL_EVENT_UNKNOWN;
        s_stkUnsolResponse = (char *)calloc((strlen(response) + 1), sizeof(char));
        snprintf(s_stkUnsolResponse, strlen(response) + 1, "%s", response);
        RLOGD("STK service is not running [%s]", s_stkUnsolResponse);
    }

    return ret;
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited (const char *s, const char *sms_pdu)
{
    char *line = NULL, *p;
    int err;

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state
     */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

#define  CGFPCCFG "%CGFPCCFG:"
    if (strStartsWith(s, CGFPCCFG)) {
        /* cuttlefish/goldfish specific
        */
        char *response;
        line = p = strdup(s);
        RLOGD("got CGFPCCFG line %s and %s\n", s, p);
        err = at_tok_start(&line);
        if(err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        }
#define kSize 5
        int configs[kSize];
        for (int i=0; i < kSize && !err; ++i) {
            err = at_tok_nextint(&line, &(configs[i]));
            RLOGD("got i %d, val = %d", i, configs[i]);
        }
        if(err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        } else {
            int modem_tech = configs[2];
            configs[2] = techFromModemType(modem_tech);
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_PHYSICAL_CHANNEL_CONFIGS,
                configs, kSize);
        }
        free(p);
    } else if (strStartsWith(s, "%CTZV:")) {
        /* TI specific -- NITZ time */
        char *response;

        line = p = strdup(s);
        at_tok_start(&p);

        err = at_tok_nextstr(&p, &response);

        if (err != 0) {
            RLOGE("invalid NITZ line %s\n", s);
        } else {
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_NITZ_TIME_RECEIVED,
                response, strlen(response) + 1);
        }
        free(line);
    } else if (strStartsWith(s,"+CRING:")
                || strStartsWith(s,"RING")
                || strStartsWith(s,"NO CARRIER")
                || strStartsWith(s,"+CCWA")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
            NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL); //TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s,"+CREG:")
                || strStartsWith(s,"+CGREG:")
                || strStartsWith(s,"+CEREG:")
                || strStartsWith(s,"+C5GREG:")
    ) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
            NULL, 0);
        RIL_requestTimedCallback(pushSignalStrengthOnce, NULL, &TIMEVAL_SIGNAL_IMMEDIATE);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CMT:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
    } else if (strStartsWith(s, "+CGEV:")) {
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extraneous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#ifdef WORKAROUND_FAKE_CGEV
    } else if (strStartsWith(s, "+CME ERROR: 150")) {
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
    } else if (strStartsWith(s, "+CTEC: ")) {
        int tech, mask;
        switch (parse_technology_response(s, &tech, NULL))
        {
            case -1: // no argument could be parsed.
                RLOGE("invalid CTEC line %s\n", s);
                break;
            case 1: // current mode correctly parsed
            case 0: // preferred mode correctly parsed
                mask = 1 << tech;
                if (mask != MDM_GSM && mask != MDM_CDMA &&
                     mask != MDM_WCDMA && mask != MDM_LTE) {
                    RLOGE("Unknown technology %d\n", tech);
                } else {
                    setRadioTechnology(sMdmInfo, tech);
                }
                break;
        }
    } else if (strStartsWith(s, "+CCSS: ")) {
        int source = 0;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+CCSS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &source) < 0) {
            RLOGE("invalid +CCSS response: %s", line);
            free(line);
            return;
        }
        SSOURCE(sMdmInfo) = source;
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED,
                                  &source, sizeof(source));
        free(line);
    } else if (strStartsWith(s, "+WSOS: ")) {
        char state = 0;
        int unsol;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WSOS: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            free(line);
            return;
        }
        if (at_tok_nextbool(&p, &state) < 0) {
            RLOGE("invalid +WSOS response: %s", line);
            free(line);
            return;
        }
        free(line);

        unsol = state ?
                RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE : RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE;

        RIL_onUnsolicitedResponse(unsol, NULL, 0);

    } else if (strStartsWith(s, "+WPRL: ")) {
        int version = -1;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WPRL: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        if (at_tok_nextint(&p, &version) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return;
        }
        free(line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_PRL_CHANGED, &version, sizeof(version));
    } else if (strStartsWith(s, "+CFUN: 0")) {
        setRadioState(RADIO_STATE_OFF);
    } else if (strStartsWith(s, "+CSQ: ")) {
        // Accept a response that is at least v6, and up to v12
        int minNumOfElements=sizeof(RIL_SignalStrength_v6)/sizeof(int);
        int maxNumOfElements=sizeof(RIL_SignalStrength_v12)/sizeof(int);
        int response[maxNumOfElements];
        memset(response, 0, sizeof(response));

        line = p = strdup(s);
        at_tok_start(&p);

        for (int count = 0; count < maxNumOfElements; count++) {
            err = at_tok_nextint(&p, &(response[count]));
            if (err < 0 && count < minNumOfElements) {
              free(line);
              return;
            }
        }

        RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
            response, sizeof(response));
        free(line);
    } else if (strStartsWith(s, "+CUSATEND")) {  // session end
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0);
    } else if (strStartsWith(s, "+CUSATP:")) {
        line = p = strdup(s);
        if (!line) {
            RLOGE("+CUSATP: Unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +CUSATP response: %s", s);
            free(line);
            return;
        }

        char *response = NULL;
        if (at_tok_nextstr(&p, &response) < 0) {
            RLOGE("%s fail", s);
            free(line);
            return;
        }

        StkUnsolEvent ret = parseProactiveCmdInd(response);
        if (ret == STK_UNSOL_EVENT_NOTIFY) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_EVENT_NOTIFY, response,
                                      strlen(response) + 1);
        } else if (ret == STK_UNSOL_PROACTIVE_CMD) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, response,
                                      strlen(response) + 1);
        }

        free(line);
    } else if (strStartsWith(s, "+REMOTEIDDISCLOSURE")) {
        RLOGD("starting REMOTEIDDISCLOSURE %s", s);
        line = p = strdup(s);
        if (!line) {
            RLOGE("+REMOTEIDDISCLOSURE unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +REMOTEIDDISCLOSURE command: %s", s);
            free(line);
            return;
        }

        RIL_CellularIdentifierDisclosure disclosure;

        if (at_tok_nextstr(&p, &disclosure.plmn) < 0) {
            RLOGE("+REMOTEIDDISCLOSURE unable to parse plmn %s", s);
            return;
        }
        if (at_tok_nextint(&p, &disclosure.identifierType) < 0) {
            RLOGE("+REMOTEIDDISCLOSURE unable to parse identifier %s", s);
            return;
        }
        if (at_tok_nextint(&p, &disclosure.protocolMessage) < 0) {
            RLOGE("+REMOTEIDDISCLOSURE unable to parse protocol message %s", s);
            return;
        }
        if (at_tok_nextbool(&p, (char*)&disclosure.isEmergency) < 0) {
            RLOGE("+REMOTEIDDISCLOSURE unable to parse isEmergency %s", s);
            return;
        }

        RIL_onUnsolicitedResponse(RIL_UNSOL_CELLULAR_IDENTIFIER_DISCLOSED, (void*)&disclosure,
                                  sizeof(disclosure));
        free(line);
    } else if (strStartsWith(s, "+UPDATESECURITYALGORITHM")) {
        RLOGD("starting UPDATESECURITYALGORITHM %s", s);
        line = p = strdup(s);
        if (!line) {
            RLOGE("+UPDATESECURITYALGORITHM unable to allocate memory");
            return;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +UPDATESECURITYALGORITHM command: %s", s);
            free(line);
            return;
        }

        RIL_SecurityAlgorithmUpdate update;

        if (at_tok_nextint(&p, &update.connectionEvent) < 0) {
            RLOGE("+UPDATESECURITYALGORITHM unable to parse connection event %s", s);
            return;
        }
        if (at_tok_nextint(&p, &update.encryption) < 0) {
            RLOGE("+UPDATESECURITYALGORITHM unable to parse encryption %s", s);
            return;
        }
        if (at_tok_nextint(&p, &update.integrity) < 0) {
            RLOGE("+UPDATESECURITYALGORITHM unable to parse integrity %s", s);
            return;
        }
        if (at_tok_nextbool(&p, (char*)&update.isUnprotectedEmergency) < 0) {
            RLOGE("+UPDATESECURITYALGORITHM unable to parse isUnprotectedEmergency %s", s);
            return;
        }

        RIL_onUnsolicitedResponse(RIL_UNSOL_SECURITY_ALGORITHM_UPDATED, &update, sizeof(update));
        free(line);
    } else {
        RLOGE("Unexpected unsolicited request: %s", s);
    }
}

/* Called on command or reader thread */
static void onATReaderClosed()
{
    RLOGI("AT channel closed\n");
    at_close();
    s_closed = 1;

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout()
{
    RLOGI("AT channel timeout; closing\n");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState (RADIO_STATE_UNAVAILABLE);
}

/* Called to pass hardware configuration information to telephony
 * framework.
 */
static void setHardwareConfiguration(int num, RIL_HardwareConfig *cfg)
{
   RIL_onUnsolicitedResponse(RIL_UNSOL_HARDWARE_CONFIG_CHANGED, cfg, num*sizeof(*cfg));
}

static void usage(char *s __unused)
{
#ifdef RIL_SHLIB
    fprintf(stderr, "reference-ril requires: -p <tcp port> or -d /dev/tty_device\n");
#else
    fprintf(stderr, "usage: %s [-p <tcp port>] [-d /dev/tty_device]\n", s);
    exit(-1);
#endif
}

static void *
mainLoop(void *param __unused)
{
    int fd;
    int ret;

    AT_DUMP("== ", "entering mainLoop()", -1 );
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        fd = -1;
        while  (fd < 0) {
            if (isInEmulator()) {
                fd = qemu_open_modem_port();
                RLOGD("opening qemu_modem_port %d!", fd);
            } else if (s_port > 0) {
                fd = socket_network_client("localhost", s_port, SOCK_STREAM);
            } else if (s_modem_simulator_port != (uint32_t)-1) {
              fd = socket(AF_VSOCK, SOCK_STREAM, 0);
              if (fd < 0) {
                 RLOGD("Can't create AF_VSOCK socket!");
                 continue;
              }
              struct sockaddr_vm sa;
              memset(&sa, 0, sizeof(struct sockaddr_vm));
              sa.svm_family = AF_VSOCK;
              sa.svm_cid = VMADDR_CID_HOST;
              sa.svm_port = s_modem_simulator_port;

              if (connect(fd, (struct sockaddr *)(&sa), sizeof(sa)) < 0) {
                  RLOGD("Can't connect to port:%ud, errno: %s",
                      s_modem_simulator_port, strerror(errno));
                  close(fd);
                  fd = -1;
                  continue;
              }
            } else if (s_device_socket) {
                fd = socket_local_client(s_device_path,
                                         ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                         SOCK_STREAM);
            } else if (s_device_path != NULL) {
                fd = open (s_device_path, O_RDWR);
                if (fd < 0) {
                    RLOGD("Can't open %s: %s", s_device_path, strerror(errno));
                    sleep(1);
                    continue;
                }
                if ( fd >= 0 && !memcmp( s_device_path, "/dev/ttyS", 9 ) ) {
                    /* disable echo on serial ports */
                    struct termios  ios;
                    tcgetattr( fd, &ios );
                    ios.c_lflag = 0;  /* disable ECHO, ICANON, etc... */
                    tcsetattr( fd, TCSANOW, &ios );
                }
            }

            if (fd < 0) {
                perror ("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            RLOGE ("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        RLOGI("Re-opening after close");
    }
}

#ifdef RIL_SHLIB

pthread_t s_tid_mainloop;

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;
    pthread_attr_t attr;

    s_rilenv = env;

    RLOGD("RIL_Init");
    while ( -1 != (opt = getopt(argc, argv, "p:d:s:c:m:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                RLOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                RLOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                RLOGI("Opening socket %s\n", s_device_path);
            break;

            case 'c':
                RLOGI("Client id received %s\n", optarg);
            break;

            case 'm':
              s_modem_simulator_port = strtoul(optarg, NULL, 10);
              RLOGI("Opening modem simulator port %ud\n", s_modem_simulator_port);
            break;

            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (s_port < 0 && s_device_path == NULL && !isInEmulator() &&
        s_modem_simulator_port == (uint32_t)-1) {
        usage(argv[0]);
        return NULL;
    }

    sMdmInfo = calloc(1, sizeof(ModemInfo));
    if (!sMdmInfo) {
        RLOGE("Unable to alloc memory for ModemInfo");
        return NULL;
    }
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);
    if (ret < 0) {
        RLOGE("pthread_create: %s:", strerror(errno));
        return NULL;
    }

    return &s_callbacks;
}
#else /* RIL_SHLIB */
int main (int argc, char **argv)
{
    int ret;
    int fd = -1;
    int opt;

    while ( -1 != (opt = getopt(argc, argv, "p:d:"))) {
        switch (opt) {
            case 'p':
                s_port = atoi(optarg);
                if (s_port == 0) {
                    usage(argv[0]);
                }
                RLOGI("Opening loopback port %d\n", s_port);
            break;

            case 'd':
                s_device_path = optarg;
                RLOGI("Opening tty device %s\n", s_device_path);
            break;

            case 's':
                s_device_path   = optarg;
                s_device_socket = 1;
                RLOGI("Opening socket %s\n", s_device_path);
            break;

            default:
                usage(argv[0]);
        }
    }

    if (s_port < 0 && s_device_path == NULL && !isInEmulator()) {
        usage(argv[0]);
    }

    RIL_register(&s_callbacks);

    mainLoop(NULL);

    return 0;
}

#endif /* RIL_SHLIB */
