/* uim.h — UIM module (SIM presence, ICCID, IMSI). */
#ifndef KEBAB_UIM_H
#define KEBAB_UIM_H

#include <stdbool.h>
#include <stddef.h>

void uim_init(void);
/* Returns true when a card is present and the ICCID was read. */
bool uim_ready(void);
/* On-demand identity refresh (safe to call from any non-client thread). */
void uim_refresh(void);
/* Cheap radio-on poll: card status only, no full identity re-read. */
void uim_poll_card(void);
void uim_get_spn(char *out, size_t max);
void uim_get_msisdn(char *out, size_t max);

/* RIL_REQUEST_SIM_IO helper (READ BINARY/transparent EF). */
int uim_sim_io(int command, int fileid, const char *path, const char *aid_hex,
               int p1, int p2, int p3, int *sw1, int *sw2, char *hex,
               size_t hex_max);

/* USIM/ISIM AKA via UIM Send APDU. auth_data_b64 is the base64 challenge
 * (RAND||AUTN); out_b64 receives the base64 response payload (RES||CK||IK).
 * Returns 0 on success and fills SW1/SW2. */
int uim_sim_auth(int auth_context, const char *auth_data_b64, const char *aid,
                 char *out_b64, size_t out_max, int *sw1, int *sw2);

#endif /* KEBAB_UIM_H */
