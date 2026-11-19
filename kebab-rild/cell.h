/* cell.h — NAS network scan and serving cell info. */
#ifndef KEBAB_CELL_H
#define KEBAB_CELL_H

#include <stdbool.h>

#include "guest/hals/ril/reference-libril/ril.h"

/* Runs NAS 0x21 and reports the result as RIL_UNSOL_NETWORK_SCAN_RESULT.
 * Safe to call from the RIL request thread: it spawns a worker. */
int cell_start_network_scan(void);

/* Builds the serving LTE cell from NAS 0x4D. Returns the cell count; *out is
 * an allocated array the caller frees (and frees the array itself). */
int cell_get_info_list(RIL_CellInfo_v12 **out);

#endif /* KEBAB_CELL_H */
