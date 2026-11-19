/*
 * cell.c — NAS network scan (M5).
 *
 * NAS 0x21 Network Scan, input TLV 0x10 network type (0 = full).
 * Output TLV 0x10 Network Information: a size-prefixed array of
 *   {u16 mcc, u16 mnc, u8 status, NUL-terminated description}
 * The array prefix width is guint16; this parser accepts either a byte count
 * or an element count and validates that it consumed the array exactly.
 * Results are delivered as RIL_UNSOL_NETWORK_SCAN_RESULT with GSM-type cells
 * (the framework only needs PLMN + technology for the operator list).
 */
#define LOG_TAG "kebab-rild"

#include "cell.h"
#include "nas.h"
#include "ril_glue.h"
#include "state.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <log/log.h>

#include "guest/hals/ril/reference-libril/ril.h"

#define NAS_NETWORK_SCAN 0x0021
#define NAS_GET_SYS_INFO 0x004D
#define MAX_SCAN_CELLS   64

static void fill_cell(RIL_CellInfo_v12 *ci, int mcc, int mnc)
{
    int digits = mnc > 99 ? 3 : 2;

    memset(ci, 0, sizeof(*ci));
    ci->cellInfoType = RIL_CELL_INFO_TYPE_GSM;
    ci->registered = 0;
    ci->timeStampType = RIL_TIMESTAMP_TYPE_MODEM;
    ci->CellInfo.gsm.cellIdentityGsm.mcc = mcc;
    ci->CellInfo.gsm.cellIdentityGsm.mnc = (digits << 12) | mnc;
    ci->CellInfo.gsm.cellIdentityGsm.lac = INT_MAX;
    ci->CellInfo.gsm.cellIdentityGsm.cid = INT_MAX;
    ci->CellInfo.gsm.signalStrengthGsm.signalStrength = 99;
    ci->CellInfo.gsm.signalStrengthGsm.bitErrorRate = 99;
}

static int parse_networks(const uint8_t *v, size_t l,
                          RIL_CellInfo_v12 *out, int max)
{
    size_t off;
    int n = 0;
    uint16_t prefix;

    if (l < 2)
        return 0;
    prefix = (uint16_t)(v[0] | (v[1] << 8));
    off = 2;

    /* Byte-count interpretation first: it must consume the array exactly. */
    {
        size_t end = off + prefix;

        if (end <= l) {
            size_t p = off;

            while (p + 5 <= end && n < max) {
                size_t slen;
                const uint8_t *str = memchr(v + p + 5, '\0', end - (p + 5));

                if (!str)
                    break;
                slen = (size_t)(str - (v + p + 5));
                fill_cell(&out[n++], v[p] | (v[p + 1] << 8),
                          v[p + 2] | (v[p + 3] << 8));
                p += 5 + slen + 1;
            }
            if (p == end)
                return n;
        }
    }

    /* Element-count interpretation. */
    {
        int i;

        for (i = 0; i < prefix && n < max; i++) {
            uint16_t mcc, mnc;
            const uint8_t *str;

            if (off + 5 > l)
                break;
            mcc = (uint16_t)(v[off] | (v[off + 1] << 8));
            mnc = (uint16_t)(v[off + 2] | (v[off + 3] << 8));
            str = memchr(v + off + 5, '\0', l - (off + 5));
            if (!str)
                break;
            fill_cell(&out[n++], mcc, mnc);
            off = (size_t)(str - v) + 1;
        }
    }
    return n;
}

static void send_result(RIL_CellInfo_v12 *infos, int n, RIL_Errno err)
{
    RIL_NetworkScanResult res;

    memset(&res, 0, sizeof(res));
    res.status = COMPLETE;
    res.network_infos_length = (uint32_t)n;
    res.network_infos = infos;
    res.error = err;
    kebab_unsol(RIL_UNSOL_NETWORK_SCAN_RESULT, &res, sizeof(res));
}

static void *scan_thread(void *arg)
{
    uint8_t req[8], rsp[4096];
    size_t n = 0, rl = sizeof(rsp);
    RIL_CellInfo_v12 *infos;
    uint16_t l = 0;
    const uint8_t *v;
    int rc, count = 0;

    (void)arg;
    if (qmi_client_wait_up(&g_nas, 20000)) {
        send_result(NULL, 0, RIL_E_RADIO_NOT_AVAILABLE);
        return NULL;
    }
    infos = calloc(MAX_SCAN_CELLS, sizeof(*infos));
    if (!infos) {
        send_result(NULL, 0, RIL_E_NO_MEMORY);
        return NULL;
    }

    n = qmi_put_u8(req, n, 0x10, 0); /* full scan */
    rc = qmi_transact(&g_nas, NAS_NETWORK_SCAN, req, n, rsp, &rl, 120000);
    if (rc) {
        ALOGE("CELL: network scan failed %d", rc);
        send_result(NULL, 0, RIL_E_GENERIC_FAILURE);
        free(infos);
        return NULL;
    }
    v = qmi_find_tlv(rsp, rl, 0x10, &l);
    if (v)
        count = parse_networks(v, l, infos, MAX_SCAN_CELLS);
    ALOGI("CELL: scan returned %d networks", count);
    send_result(infos, count, RIL_E_SUCCESS);
    free(infos);
    return NULL;
}

int cell_start_network_scan(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, scan_thread, NULL) != 0) {
        ALOGE("CELL: cannot start scan thread");
        return -errno;
    }
    pthread_detach(t);
    return 0;
}

/*
 * NAS 0x4D "LTE System Info v2" (TLV 0x19), packed sequence:
 *   [domain v][domain] [svc v][svc] [roam v][roam] [forb v][forb]
 *   [lac v][lac u16] [cid v][cid u32] [rej v][rej dom][rej cause]
 *   [netid v][MCC str][MNC str] [tac v][tac u16]
 */
struct lte_sysinfo {
    int mcc, mnc, lac, cid, tac;
    bool valid;
};

static bool parse_lte_sysinfo(const uint8_t *v, size_t l, struct lte_sysinfo *out)
{
    size_t p = 0;
    int lac = 0, cid = 0, tac = 0, mcc = 0, mnc = 0;

    memset(out, 0, sizeof(*out));
    if (l < 20)
        return false;

    p += 1; /* domain valid */
    p += 1;
    p += 1; /* service capability valid */
    p += 1;
    p += 1; /* roaming valid */
    p += 1;
    p += 1; /* forbidden valid */
    p += 1;
    if (p + 3 > l)
        return false;
    if (v[p++]) { /* LAC valid */
        lac = v[p] | (v[p + 1] << 8);
    }
    p += 2;
    if (p + 5 > l)
        return false;
    if (v[p++]) { /* CID valid */
        cid = v[p] | (v[p + 1] << 8) | (v[p + 2] << 16) | (v[p + 3] << 24);
    }
    p += 4;
    p += 3; /* reject info valid/domain/cause */
    if (p + 1 > l)
        return false;
    p += 1; /* network id valid */
    /* MCC and MNC are always present as NUL-terminated strings. */
    {
        const uint8_t *s = memchr(v + p, '\0', l - p);

        if (!s)
            return false;
        mcc = atoi((const char *)(v + p));
        p = (size_t)(s - v) + 1;
        s = memchr(v + p, '\0', l - p);
        if (!s)
            return false;
        mnc = atoi((const char *)v + p);
        p = (size_t)(s - v) + 1;
    }
    if (p + 1 <= l && v[p++]) { /* TAC valid */
        if (p + 2 <= l)
            tac = v[p] | (v[p + 1] << 8);
    }
    out->mcc = mcc;
    out->mnc = mnc;
    out->lac = lac;
    out->cid = cid;
    out->tac = tac;
    out->valid = mcc > 0;
    return out->valid;
}

int cell_get_info_list(RIL_CellInfo_v12 **out)
{
    uint8_t rsp[1024];
    size_t rl = sizeof(rsp);
    uint16_t l = 0;
    const uint8_t *v;
    RIL_CellInfo_v12 *ci;
    struct lte_sysinfo si;
    int rssi = 0, rsrp = 0, rsrq = 0, snr = 0;
    int digits;

    *out = NULL;
    if (!qmi_client_is_up(&g_nas))
        return 0;
    if (qmi_transact(&g_nas, NAS_GET_SYS_INFO, NULL, 0, rsp, &rl, 10000))
        return 0;
    v = qmi_find_tlv(rsp, rl, 0x19, &l);
    if (!v || !parse_lte_sysinfo(v, l, &si))
        return 0;

    ci = calloc(1, sizeof(*ci));
    if (!ci)
        return 0;
    digits = si.mnc > 99 ? 3 : 2;
    ci->cellInfoType = RIL_CELL_INFO_TYPE_LTE;
    ci->registered = 1;
    ci->timeStampType = RIL_TIMESTAMP_TYPE_MODEM;
    ci->CellInfo.lte.cellIdentityLte.mcc = si.mcc;
    ci->CellInfo.lte.cellIdentityLte.mnc = (digits << 12) | si.mnc;
    ci->CellInfo.lte.cellIdentityLte.ci = si.cid ? si.cid : INT_MAX;
    ci->CellInfo.lte.cellIdentityLte.tac = si.tac ? si.tac : INT_MAX;
    /* PCI and EARFCN are not in Get System Info; stay honest and mark unknown. */
    ci->CellInfo.lte.cellIdentityLte.pci = INT_MAX;
    ci->CellInfo.lte.cellIdentityLte.earfcn = INT_MAX;

    if (nas_get_signal(&rssi, &rsrp, &rsrq, &snr)) {
        int asu = (rssi + 113) / 2;
        int rsrp_dbm = rsrp / 10; /* NAS reports 0.1 dBm */

        ci->CellInfo.lte.signalStrengthLte.signalStrength =
            asu < 0 ? 0 : asu > 31 ? 31 : asu;
        if (rsrp_dbm <= -44 && rsrp_dbm >= -140)
            ci->CellInfo.lte.signalStrengthLte.rsrp = -rsrp_dbm;
        else
            ci->CellInfo.lte.signalStrengthLte.rsrp = INT_MAX;
        if (rsrq <= -3 && rsrq >= -20)
            ci->CellInfo.lte.signalStrengthLte.rsrq = -rsrq;
        else
            ci->CellInfo.lte.signalStrengthLte.rsrq = INT_MAX;
        ci->CellInfo.lte.signalStrengthLte.rssnr =
            (snr >= -200 && snr <= 300) ? snr : INT_MAX;
    } else {
        ci->CellInfo.lte.signalStrengthLte.signalStrength = 99;
        ci->CellInfo.lte.signalStrengthLte.rsrp = INT_MAX;
        ci->CellInfo.lte.signalStrengthLte.rsrq = INT_MAX;
        ci->CellInfo.lte.signalStrengthLte.rssnr = INT_MAX;
    }
    ci->CellInfo.lte.signalStrengthLte.cqi = INT_MAX;
    ci->CellInfo.lte.signalStrengthLte.timingAdvance = INT_MAX;

    *out = ci;
    return 1;
}
