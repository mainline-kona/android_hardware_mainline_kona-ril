/*
 * qmi.h — minimal QMI-over-QRTR transport for kebab-rild.
 *
 * One client per QMI service, addressed over AF_QIPCRTR. The client thread
 * owns the socket, keeps the QRTR NEW_LOOKUP subscription alive across modem
 * restarts and dispatches indications; callers use qmi_transact() for
 * request/response messages. Payloads are raw QMI TLVs.
 *
 * NOTE: this is the transport seam. Every RIL module talks to the modem only
 * through this API, so a typed libqmi backend can replace it without touching
 * the modules.
 */
#ifndef KEBAB_QMI_H
#define KEBAB_QMI_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* QMI service ids */
#define QMI_SVC_WDS   0x01
#define QMI_SVC_DMS   0x02
#define QMI_SVC_NAS   0x03
#define QMI_SVC_WMS   0x05
#define QMI_SVC_VOICE 0x09
#define QMI_SVC_UIM   0x0b
#define QMI_SVC_IMSA  0x21
#define QMI_SVC_IMSS  0x22
#define QMI_SVC_DSD   0x2a

#define QMI_MAX_MSG 4096

struct qmi_client;

typedef void (*qmi_ind_cb)(struct qmi_client *c, uint16_t msg,
                           const uint8_t *tlvs, size_t len);
typedef void (*qmi_up_cb)(struct qmi_client *c, bool up);

/*
 * Indication and up/down callbacks run on the client's own thread. They must
 * not call qmi_transact() on the same client (the response would be processed
 * by that same thread) — only update state and notify other modules.
 */

struct qmi_client {
    uint16_t service;
    qmi_ind_cb ind_cb;
    qmi_up_cb up_cb;

    int fd;
    pthread_t thread;
    bool running;

    pthread_mutex_t mu;      /* protects port/node/wait fields */
    pthread_cond_t cv;
    pthread_mutex_t req_mu;  /* serializes transactions */
    pthread_mutex_t snd_mu;  /* serializes socket writes */

    uint32_t node;
    uint32_t port;           /* 0 = service not up */
    uint16_t txn;
    uint16_t wait_txn;
    uint16_t wait_msg;
    bool wait_done;
    size_t wait_len;
    uint8_t wait_rsp[QMI_MAX_MSG];
};

/* Starts the client thread. Returns 0 on success. */
int qmi_client_start(struct qmi_client *c, uint16_t service,
                     qmi_ind_cb ind_cb, qmi_up_cb up_cb);

/* Waits until the service is announced by QRTR. */
int qmi_client_wait_up(struct qmi_client *c, int timeout_ms);

bool qmi_client_is_up(struct qmi_client *c);

/*
 * Sends one request and waits for its response. rsp/rsp_len may be NULL when
 * the reply body is not needed. Returns 0 on success, negative errno on
 * timeout/transport failure. A QMI result/error TLV is parsed and returned as
 * the positive QMI error code (0 on success).
 */
int qmi_transact(struct qmi_client *c, uint16_t msgid,
                 const uint8_t *tlvs, size_t tlen,
                 uint8_t *rsp, size_t *rsp_len, int timeout_ms);

/* TLV helpers */
size_t qmi_put_tlv(uint8_t *buf, size_t off, uint8_t type,
                   const void *data, uint16_t len);
size_t qmi_put_u8(uint8_t *buf, size_t off, uint8_t type, uint8_t v);
size_t qmi_put_u16(uint8_t *buf, size_t off, uint8_t type, uint16_t v);
size_t qmi_put_u32(uint8_t *buf, size_t off, uint8_t type, uint32_t v);
const uint8_t *qmi_find_tlv(const uint8_t *tlvs, size_t len, uint8_t type,
                            uint16_t *out_len);

/*
 * Test seams (weak in qmi.c, overridden by the host test binary).
 * qmi_test_transact returns -ENOSYS when it does not handle the message;
 * otherwise its non-negative return value is the QMI error code and the reply
 * TLVs are already stored in rsp/rsp_len.
 * qmi_test_is_up forces a service up without a QRTR announcement.
 */
int qmi_test_transact(struct qmi_client *c, uint16_t msgid,
                      const uint8_t *req, size_t reqlen,
                      uint8_t *rsp, size_t rsp_cap, size_t *rsp_len);
bool qmi_test_is_up(struct qmi_client *c);

/* Test-only: run the client's indication callback as the client thread would. */
void qmi_test_deliver_indication(struct qmi_client *c, uint16_t msg,
                                 const uint8_t *tlvs, size_t len);

#endif /* KEBAB_QMI_H */
