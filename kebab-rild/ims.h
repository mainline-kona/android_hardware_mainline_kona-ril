/* ims.h — IMSA registration tracking. */
#ifndef KEBAB_IMS_H
#define KEBAB_IMS_H

#include <stdbool.h>

void ims_init(void);

/* True when IMSA reports fully registered (QMI status 2). */
bool ims_voip_registered(void);
/* 0 = not registered, 1 = registering, 2 = registered. */
int ims_reg_status(void);

/* One synchronous registration/service poll (also used by the poll thread). */
void ims_poll_once(void);

/* Radio off / airplane mode: force unregistered and notify the framework. */
void ims_force_unregistered(void);

#endif /* KEBAB_IMS_H */
