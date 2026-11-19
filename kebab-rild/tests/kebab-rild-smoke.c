/* Host-side smoke test: drives the kebab-rild RIL entry points without a modem. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "guest/hals/ril/reference-libril/ril.h"

static int completions;
static int unsols;

static void on_complete(RIL_Token t, RIL_Errno e, void *resp, size_t len)
{
    printf("complete token=%ld err=%d len=%zu\n", (long)t, e, len);
    completions++;
}

static void on_unsol(int response, const void *data, size_t len)
{
    (void)data;
    (void)len;
    printf("unsol %d\n", response);
    unsols++;
}

static void timed_cb(RIL_TimedCallback cb, void *param,
                     const struct timeval *tv)
{
    (void)cb;
    (void)param;
    (void)tv;
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc,
                                   char **argv);

int smoke_main(void)
{
    struct RIL_Env env = {
        .OnRequestComplete = on_complete,
        .OnUnsolicitedResponse = on_unsol,
        .RequestTimedCallback = timed_cb,
    };
    const RIL_RadioFunctions *f = RIL_Init(&env, 0, NULL);
    int on = 0, n = 0;
    char *imsi_args = NULL;

    if (!f) {
        printf("RIL_Init returned NULL\n");
        return 1;
    }
    printf("version=%s\n", f->getVersion());

    n = 0;
    f->onRequest(RIL_REQUEST_GET_SIM_STATUS, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_GET_IMSI, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_OPERATOR, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_VOICE_REGISTRATION_STATE, NULL, 0,
                 (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_SIGNAL_STRENGTH, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_DATA_CALL_LIST, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_GET_CURRENT_CALLS, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_GET_RADIO_CAPABILITY, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_BASEBAND_VERSION, NULL, 0, (RIL_Token)(long)++n);
    f->onRequest(RIL_REQUEST_IMS_REGISTRATION_STATE, NULL, 0, (RIL_Token)(long)++n);
    (void)imsi_args;
    /* RADIO_POWER=0 is immediate (no DMS wait); must still complete. */
    f->onRequest(RIL_REQUEST_RADIO_POWER, &on, sizeof(on), (RIL_Token)(long)++n);

    /* RADIO_POWER waits for the DMS client; poll instead of a fixed 20 s. */
    for (int i = 0; i < 200 && completions < n; i++)
        usleep(100000);
    printf("completions=%d unsols=%d requests=%d\n", completions, unsols, n);
    return completions == n ? 0 : 2;
}
