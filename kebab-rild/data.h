/* data.h — WDS data module (multi-PDN table). */
#ifndef KEBAB_DATA_H
#define KEBAB_DATA_H

#include "guest/hals/ril/reference-libril/ril.h"

#define DATA_MAX_CALLS 4

void data_init(void);

/* Initial attach APN from RIL_SET_INITIAL_ATTACH_APN, used when a setup
 * request carries no APN. */
void data_set_initial_attach_apn(const char *apn);

/* Starts a PDN and fills its RIL response. Returns 0 on success. */
int data_setup(const char *apn, const char *pdp_type, const char *roaming_type,
               const char *user, const char *password, int auth,
               int profile, RIL_Data_Call_Response_v11 *out);

/* Stops one call by cid; -ENOENT if unknown. */
int data_deactivate_cid(int cid);
/* Stops all active calls (QMI), used on shutdown. */
void data_deactivate_all(void);
/* Drops all calls without QMI transactions (modem restart). */
void data_clear_all(void);
/* Copies active calls; returns the number written (<= max). */
int data_snapshot_all(RIL_Data_Call_Response_v11 *out, int max);

#endif /* KEBAB_DATA_H */
