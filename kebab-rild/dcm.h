/* dcm.h — QMI IMS DCM (service 0x302) server, optional in-daemon. */
#ifndef KEBAB_DCM_H
#define KEBAB_DCM_H

/* Starts the DCM server when persist.vendor.kebab.dcm_in_rild=1.
 * Returns 0 when the server thread was started (or intentionally disabled
 * and another process owns the service). */
int dcm_init(void);

#endif /* KEBAB_DCM_H */
