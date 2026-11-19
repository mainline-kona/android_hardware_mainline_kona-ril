/* nas.h — NAS module (serving system, signal, system selection). */
#ifndef KEBAB_NAS_H
#define KEBAB_NAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "guest/hals/ril/reference-libril/ril.h"

void nas_init(void);
/* Fetch serving system + signal in a worker thread. */
void nas_refresh(void);

/* Snapshot helpers; return false when no fresh data is available. */
bool nas_get_serving(int *reg_state, int *ps_state, int *radio_if,
                     int *mcc, int *mnc, int *mnc_len);
bool nas_get_signal(int *rssi, int *rsrp, int *rsrq, int *snr);
/* Builds a RIL v12 signal report with the correct per-RAT units. */
void nas_build_signal_strength(RIL_SignalStrength_v12 *ss);
void nas_get_op_name(char *out, size_t max);
void nas_get_op_short(char *out, size_t max);

/* NAS 0x33: LTE + NR5G, automatic selection, PS domain only, immediate. */
int nas_apply_system_selection(void);
/* Retries 0x33 in a worker until the NAS service accepts it (boot/SSR). */
void nas_apply_system_selection_retry(void);

/* Register NAS event/indication masks in a worker (stock does this per start). */
void nas_register_indications(void);

/* Radio off: invalidate cached registration/signal/operator state. */
void nas_clear_state(void);

#endif /* KEBAB_NAS_H */
