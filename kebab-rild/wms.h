/* wms.h — SMS over QMI WMS (send, receive, ack). */
#ifndef KEBAB_WMS_H
#define KEBAB_WMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void wms_init(void);
bool wms_get_smsc(char *out, size_t max);

/*
 * Sends one SMS PDU. Returns:
 *   0 = sent (msg_ref filled)
 *   1 = modem rejected the message (rp_cause filled)
 *   2 = retryable (IMS transport not up yet)
 *  <0 = transport/other error
 */
int wms_send_sms(const char *smsc_hex, const char *pdu_hex, bool on_ims,
                 int *msg_ref, int *rp_cause);

/* RIL_REQUEST_SMS_ACKNOWLEDGE; returns true if it belonged to a QMI message. */
bool wms_ack_sms(int success, int fail_cause);

/* True when WMS reports the IMS SMS transport as available. */
bool wms_ims_transport(void);

/* Radio transition: drop transport state on off, reconfigure on on. */
void wms_radio_changed(bool on);

#endif /* KEBAB_WMS_H */
