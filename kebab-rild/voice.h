/* voice.h — QMI voice module (dial, answer, end, DTMF, call list). */
#ifndef KEBAB_VOICE_H
#define KEBAB_VOICE_H

#include "guest/hals/ril/reference-libril/ril.h"

void voice_init(void);

/* Spawns/stops the single CS-VOICE helper (mutex-gated). */
void voice_path_set(int on);
int voice_path_wait(int timeout_ms);

int voice_dial(const char *number, int clir, int *call_id);
int voice_dial_emergency(const char *number, int clir, int *call_id);
int voice_get_last_fail_cause(void);
int voice_end(int call_id);
int voice_answer(int call_id);
int voice_dtmf(int call_id, const char *digits);
/* SS/USSD (M5). */
int voice_ussd(const char *text, int dcs);
int voice_ussd_cancel(void);

/* Allocates a RIL_Call** array (caller frees each call and the array). */
int voice_get_calls(RIL_Call ***out);

/* Drops local call state and notifies the framework (radio off / modem SSR). */
void voice_clear_calls(void);

#endif /* KEBAB_VOICE_H */
