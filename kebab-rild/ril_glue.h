/* ril_glue.h — RIL environment access for modules that only send unsoliciteds. */
#ifndef KEBAB_RIL_GLUE_H
#define KEBAB_RIL_GLUE_H

#include <stdbool.h>
#include <stddef.h>

void kebab_unsol(int response, const void *data, size_t datalen);

/* Transport-level modem transition (DMS service down/up). Updates the RIL
 * radio state and notifies the framework. Safe from any non-RIL thread. */
void kebab_modem_state_changed(bool up);

#endif /* KEBAB_RIL_GLUE_H */
