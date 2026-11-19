/* dms.h — DMS module (operating mode). */
#ifndef KEBAB_DMS_H
#define KEBAB_DMS_H

#include <stdbool.h>
#include <stddef.h>

void dms_init(void);
/* DMS 0x2E Set Operating Mode: online=0, low power=1. Error 52 = success. */
int dms_set_online(bool online);

struct dms_ids {
    char imei[24];
    char imeisv[24];
    char meid[24];
    char esn[16];
};

/* DMS 0x0025 Get IDs (TLVs 0x11/0x13/0x12/0x10). Returns 0 when IMEI is present. */
int dms_get_ids(struct dms_ids *out);
/* DMS 0x0023 Get Revision (baseband version). */
int dms_get_revision(char *out, size_t max);

#endif /* KEBAB_DMS_H */
