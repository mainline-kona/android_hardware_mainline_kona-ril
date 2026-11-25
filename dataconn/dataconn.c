/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Brings up one mobile data session on the modem and holds it until SIGTERM.
 *
 * Supports PCIe MHI endpoints (wwan0 / rmnet_mhi0 on SM8250+SDX55 Kona) with fallback
 * to embedded IPA endpoints (rmnet_ipa0).
 *
 * Usage: kona-dataconn <apn> <ipv4|ipv6|ipv4v6> [mux_id]
 * Prints key=value lines (ifname, address, ipv6_address, gateway, dns, pcscf, mtu)
 * and then "ready" once the session is up, or "error=<reason>" and exits non-zero.
 * Prints "disconnected" and exits when the modem drops the session.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glib-unix.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define LINK_PREFIX "rmnet_data"

typedef struct {
    GMainLoop *loop;
    GAsyncResult *res;
} Wait;

static void on_done(GObject *source, GAsyncResult *res, gpointer data) {
    (void)source;
    Wait *w = data;
    w->res = g_object_ref(res);
    g_main_loop_quit(w->loop);
}

static GAsyncResult *wait_result(Wait *w) {
    g_clear_object(&w->res);
    g_main_loop_run(w->loop);
    return w->res;
}

static void fail(const char *what, GError *error) {
    printf("error=%s: %s\n", what, error ? error->message : "unknown");
    fflush(stdout);
    exit(1);
}

static guint read_sysfs_uint(const char *path) {
    g_autofree gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) return 0;
    return (guint)strtoul(contents, NULL, 0);
}

static const char *get_parent_iface(void) {
    for (int retry = 0; retry < 10; retry++) {
        if (if_nametoindex("mhi_hwip0") > 0) return "mhi_hwip0";
        if (if_nametoindex("wwan0") > 0) return "wwan0";
        if (if_nametoindex("rmnet_mhi0") > 0) return "rmnet_mhi0";
        if (if_nametoindex("rmnet_ipa0") > 0) return "rmnet_ipa0";
        usleep(100000);
    }
    return "mhi_hwip0";
}

static void get_endpoint_info(const char *parent, QmiDataEndpointType *ep_type, guint *ep_index) {
    if (strcmp(parent, "rmnet_ipa0") == 0) {
        *ep_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED;
        *ep_index = 1;
    } else {
        /* External modem over PCIe (SDX55 on Kona, IP_HW0) */
        *ep_type = QMI_DATA_ENDPOINT_TYPE_PCIE;
        *ep_index = 4;
    }
}

static void set_iface_up(const char *ifname) {
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) printf("warning=set %s up: %s\n", ifname, strerror(errno));
    }
    close(fd);
}

static void set_iface_mtu(const char *ifname, guint32 mtu) {
    if (!mtu) return;
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) printf("warning=set mtu: %s\n", strerror(errno));
    close(fd);
}

static void configure_ipv4(const char *ifname, guint32 addr, guint32 mask) {
    struct ifreq ifr;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) printf("warning=set address: %s\n", strerror(errno));
    sin->sin_addr.s_addr = htonl(mask);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) printf("warning=set netmask: %s\n", strerror(errno));
    close(fd);
}

static void configure_ipv6(const char *ifname, const struct in6_addr *addr, guint8 prefix) {
    unsigned int ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        printf("warning=if_nametoindex %s failed\n", ifname);
        return;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        printf("warning=netlink socket: %s\n", strerror(errno));
        return;
    }

    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
        char attrbuf[128];
    } req;
    memset(&req, 0, sizeof(req));

    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nh.nlmsg_type = RTM_NEWADDR;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
    req.nh.nlmsg_seq = 1;

    req.ifa.ifa_family = AF_INET6;
    req.ifa.ifa_prefixlen = prefix;
    req.ifa.ifa_flags = IFA_F_NODAD;
    req.ifa.ifa_scope = RT_SCOPE_UNIVERSE;
    req.ifa.ifa_index = ifindex;

    struct rtattr *rta = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = IFA_LOCAL;
    rta->rta_len = RTA_LENGTH(sizeof(*addr));
    memcpy(RTA_DATA(rta), addr, sizeof(*addr));
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    rta = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = IFA_ADDRESS;
    rta->rta_len = RTA_LENGTH(sizeof(*addr));
    memcpy(RTA_DATA(rta), addr, sizeof(*addr));
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
        printf("warning=netlink send ipv6: %s\n", strerror(errno));
    }
    close(fd);
}

static QmiClient *allocate(QmiDevice *dev, QmiService service, Wait *w) {
    GError *error = NULL;
    qmi_device_allocate_client(dev, service, QMI_CID_NONE, 10, NULL, on_done, w);
    QmiClient *client = qmi_device_allocate_client_finish(dev, wait_result(w), &error);
    if (!client) fail("allocate client", error);
    return client;
}

static void print_ipv4(const char *key, guint32 addr) {
    struct in_addr in = {.s_addr = htonl(addr)};
    printf("%s=%s\n", key, inet_ntoa(in));
}

static gboolean on_sigterm(gpointer loop) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean s_disconnected;

static void on_packet_service_status(QmiClientWds *wds, QmiIndicationWdsPacketServiceStatusOutput *ind,
                                     gpointer loop) {
    (void)wds;
    QmiWdsConnectionStatus status;
    if (!qmi_indication_wds_packet_service_status_output_get_connection_status(ind, &status, NULL, NULL) ||
            status != QMI_WDS_CONNECTION_STATUS_DISCONNECTED)
        return;
    s_disconnected = TRUE;
    printf("disconnected\n");
    fflush(stdout);
    g_main_loop_quit(loop);
}

static QrtrNode *find_wds_node(QrtrBus *bus) {
    GList *nodes = qrtr_bus_peek_nodes(bus);
    for (GList *l = nodes; l != NULL; l = l->next) {
        QrtrNode *n = QRTR_NODE(l->data);
        if (qrtr_node_lookup_port(n, QMI_SERVICE_WDS) >= 0) {
            return g_object_ref(n);
        }
    }
    return NULL;
}

static void release_client(QmiDevice *dev, QmiClient *client, Wait *w) {
    if (!client) return;
    GError *error = NULL;
    qmi_device_release_client(dev, client, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 5, NULL,
                              on_done, w);
    qmi_device_release_client_finish(dev, wait_result(w), &error);
    g_clear_error(&error);
    g_object_unref(client);
}

static void ensure_modem_online(QmiDevice *dev, Wait *w) {
    GError *error = NULL;
    qmi_device_allocate_client(dev, QMI_SERVICE_DMS, QMI_CID_NONE, 10, NULL, on_done, w);
    QmiClient *dms = qmi_device_allocate_client_finish(dev, wait_result(w), &error);
    if (!dms) {
        g_clear_error(&error);
        return;
    }
    qmi_client_dms_get_operating_mode(QMI_CLIENT_DMS(dms), NULL, 10, NULL, on_done, w);
    g_autoptr(QmiMessageDmsGetOperatingModeOutput) out =
            qmi_client_dms_get_operating_mode_finish(QMI_CLIENT_DMS(dms), wait_result(w), &error);
    if (out && qmi_message_dms_get_operating_mode_output_get_result(out, &error)) {
        QmiDmsOperatingMode mode = QMI_DMS_OPERATING_MODE_ONLINE;
        qmi_message_dms_get_operating_mode_output_get_mode(out, &mode, NULL);
        if (mode != QMI_DMS_OPERATING_MODE_ONLINE) {
            g_autoptr(QmiMessageDmsSetOperatingModeInput) sin =
                    qmi_message_dms_set_operating_mode_input_new();
            qmi_message_dms_set_operating_mode_input_set_mode(sin, QMI_DMS_OPERATING_MODE_ONLINE,
                                                              NULL);
            qmi_client_dms_set_operating_mode(QMI_CLIENT_DMS(dms), sin, 10, NULL, on_done, w);
            g_autoptr(QmiMessageDmsSetOperatingModeOutput) sout =
                    qmi_client_dms_set_operating_mode_finish(QMI_CLIENT_DMS(dms), wait_result(w),
                                                             &error);
            if (!sout || !qmi_message_dms_set_operating_mode_output_get_result(sout, &error)) {
                printf("warning=dms set online: %s\n", error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
    } else {
        g_clear_error(&error);
    }
    release_client(dev, dms, w);
}

/*
 * Query raw WDS GetCurrentSettings (0x002D) TLV 0x2E for IPv6 P-CSCF addresses,
 * which libqmi's generated WDS JSON does not expose as a typed getter.
 */
static void print_ipv6_pcscf(QmiDevice *dev, QmiClient *wds6, Wait *w) {
    GError *error = NULL;
    g_autoptr(QmiMessage) msg =
            qmi_message_new(QMI_SERVICE_WDS, qmi_client_get_cid(wds6), 0, 0x002D);
    gsize tlv = qmi_message_tlv_write_init(msg, 0x10, NULL);
    qmi_message_tlv_write_guint32(
            msg, QMI_ENDIAN_LITTLE,
            QMI_WDS_REQUESTED_SETTINGS_PCSCF_SERVER_ADDRESS_LIST |
                    QMI_WDS_REQUESTED_SETTINGS_PCSCF_DOMAIN_NAME_LIST,
            NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);

    qmi_device_command_full(dev, msg, NULL, 10, NULL, on_done, w);
    g_autoptr(QmiMessage) resp = qmi_device_command_full_finish(dev, wait_result(w), &error);
    if (!resp) {
        g_clear_error(&error);
        return;
    }
    guint16 tlv_len = 0;
    gsize init = qmi_message_tlv_read_init(resp, 0x2E, &tlv_len, NULL);
    if (!init || tlv_len < 17) return;
    gsize off = 0;
    guint8 count = 0;
    if (!qmi_message_tlv_read_guint8(resp, init, &off, &count, NULL)) return;
    for (guint8 i = 0; i < count && (off + 16) <= tlv_len; i++) {
        struct in6_addr addr6;
        for (int j = 0; j < 16; j++) {
            guint8 b = 0;
            if (!qmi_message_tlv_read_guint8(resp, init, &off, &b, NULL)) return;
            addr6.s6_addr[j] = b;
        }
        char buf6[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &addr6, buf6, sizeof(buf6))) {
            printf("pcscf=%s\n", buf6);
        }
    }
}

int main(int argc, char **argv) {
    GError *error = NULL;
    Wait w = {.loop = g_main_loop_new(NULL, FALSE)};
    const char *apn = argc > 1 ? argv[1] : "";
    const char *protocol = argc > 2 ? argv[2] : "ipv4";
    guint req_mux_id = argc > 3 ? (guint)atoi(argv[3]) : 1;
    if (req_mux_id == 0) req_mux_id = 1;

    qrtr_bus_new(1000, NULL, on_done, &w);
    g_autoptr(QrtrBus) bus = qrtr_bus_new_finish(wait_result(&w), &error);
    if (!bus) fail("open qrtr bus", error);

    /*
     * On OnePlus 8T (SM8250 + external SDX55 over MHI), the modem exposes
     * QMI WDS/WDA on QRTR node 3 rather than node 0. Prefer any node already
     * advertising QMI_SERVICE_WDS, then fall back to node 3.
     */
    g_autoptr(QrtrNode) node = find_wds_node(bus);
    if (!node) {
        qrtr_bus_wait_for_node(bus, 3, 30000, NULL, on_done, &w);
        node = qrtr_bus_wait_for_node_finish(bus, wait_result(&w), &error);
    }
    if (!node) fail("find modem node", error);

    qmi_device_new_from_node(node, NULL, on_done, &w);
    g_autoptr(QmiDevice) dev = qmi_device_new_from_node_finish(wait_result(&w), &error);
    if (!dev) fail("open device", error);

    qmi_device_open(dev, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, on_done, &w);
    if (!qmi_device_open_finish(dev, wait_result(&w), &error)) fail("open device", error);

    ensure_modem_online(dev, &w);

    const char *parent = get_parent_iface();
    QmiDataEndpointType ep_type = QMI_DATA_ENDPOINT_TYPE_PCIE;
    guint ep_index = 4;
    get_endpoint_info(parent, &ep_type, &ep_index);

    gboolean want_v4 = (strcasecmp(protocol, "ipv4") == 0 || strcasecmp(protocol, "ipv4v6") == 0);
    gboolean want_v6 = (strcasecmp(protocol, "ipv6") == 0 || strcasecmp(protocol, "ipv4v6") == 0);
    if (!want_v4 && !want_v6) want_v4 = TRUE;

    QmiClient *wda = allocate(dev, QMI_SERVICE_WDA, &w);
    QmiClient *wds4 = want_v4 ? allocate(dev, QMI_SERVICE_WDS, &w) : NULL;
    QmiClient *wds6 = want_v6 ? allocate(dev, QMI_SERVICE_WDS, &w) : NULL;

    /* IPA only: the modem's TX endpoint feeds IPA's RX port and the other way round */
    if (strcmp(parent, "rmnet_ipa0") == 0) {
        QmiClient *dpm = allocate(dev, QMI_SERVICE_DPM, &w);
        char sysfs_tx[128], sysfs_rx[128];
        snprintf(sysfs_tx, sizeof(sysfs_tx), "/sys/class/net/%s/device/modem/tx_endpoint_id", parent);
        snprintf(sysfs_rx, sizeof(sysfs_rx), "/sys/class/net/%s/device/modem/rx_endpoint_id", parent);
        guint tx_id = read_sysfs_uint(sysfs_tx);
        guint rx_id = read_sysfs_uint(sysfs_rx);
        if (tx_id && rx_id) {
            QmiMessageDpmOpenPortInputHardwareDataPortsElement port = {
                .endpoint_type = ep_type,
                .interface_number = ep_index,
                .rx_endpoint_number = tx_id,
                .tx_endpoint_number = rx_id,
            };
            g_autoptr(GArray) ports =
                    g_array_new(FALSE, FALSE, sizeof(QmiMessageDpmOpenPortInputHardwareDataPortsElement));
            g_array_append_val(ports, port);
            g_autoptr(QmiMessageDpmOpenPortInput) in = qmi_message_dpm_open_port_input_new();
            qmi_message_dpm_open_port_input_set_hardware_data_ports(in, ports, NULL);
            qmi_client_dpm_open_port(QMI_CLIENT_DPM(dpm), in, 10, NULL, on_done, &w);
            g_autoptr(QmiMessageDpmOpenPortOutput) out =
                    qmi_client_dpm_open_port_finish(QMI_CLIENT_DPM(dpm), wait_result(&w), &error);
            /* A port left open by an earlier session is fine */
            if (!out || !qmi_message_dpm_open_port_output_get_result(out, &error)) g_clear_error(&error);
        }
    }

    /*
     * Check current WDA format first: when a second APN (e.g. ims) comes up
     * alongside an active internet session, re-sending WDA_SET_DATA_FORMAT can
     * fail with DeviceInUse or disrupt the active muxed link.
     */
    {
        gboolean need_set = TRUE;
        g_autoptr(QmiMessageWdaGetDataFormatInput) gin =
                qmi_message_wda_get_data_format_input_new();
        qmi_message_wda_get_data_format_input_set_endpoint_info(gin, ep_type, ep_index, NULL);
        qmi_client_wda_get_data_format(QMI_CLIENT_WDA(wda), gin, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdaGetDataFormatOutput) gout =
                qmi_client_wda_get_data_format_finish(QMI_CLIENT_WDA(wda), wait_result(&w), &error);
        if (gout && qmi_message_wda_get_data_format_output_get_result(gout, &error)) {
            QmiWdaLinkLayerProtocol llp = QMI_WDA_LINK_LAYER_PROTOCOL_UNKNOWN;
            QmiWdaDataAggregationProtocol ul = QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED;
            QmiWdaDataAggregationProtocol dl = QMI_WDA_DATA_AGGREGATION_PROTOCOL_DISABLED;
            qmi_message_wda_get_data_format_output_get_link_layer_protocol(gout, &llp, NULL);
            qmi_message_wda_get_data_format_output_get_uplink_data_aggregation_protocol(gout, &ul,
                                                                                        NULL);
            qmi_message_wda_get_data_format_output_get_downlink_data_aggregation_protocol(gout, &dl,
                                                                                          NULL);
            if (llp == QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP &&
                ul == QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5 &&
                dl == QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5) {
                need_set = FALSE;
            }
        } else {
            g_clear_error(&error);
        }

        if (need_set) {
            g_autoptr(QmiMessageWdaSetDataFormatInput) in =
                    qmi_message_wda_set_data_format_input_new();
            qmi_message_wda_set_data_format_input_set_link_layer_protocol(
                    in, QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, NULL);
            qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(
                    in, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5, NULL);
            qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(
                    in, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5, NULL);
            qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_size(in, 32768,
                                                                                         NULL);
            qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_datagrams(
                    in, 32, NULL);
            qmi_message_wda_set_data_format_input_set_endpoint_info(in, ep_type, ep_index, NULL);
            qmi_client_wda_set_data_format(QMI_CLIENT_WDA(wda), in, 10, NULL, on_done, &w);
            g_autoptr(QmiMessageWdaSetDataFormatOutput) out =
                    qmi_client_wda_set_data_format_finish(QMI_CLIENT_WDA(wda), wait_result(&w),
                                                          &error);
            if (!out || !qmi_message_wda_set_data_format_output_get_result(out, &error))
                fail("set data format", error);
        }
    }

    set_iface_up(parent);

    qmi_device_add_link_with_flags(dev, req_mux_id, parent, LINK_PREFIX,
                                   QMI_DEVICE_ADD_LINK_FLAGS_INGRESS_MAP_CKSUMV5 |
                                           QMI_DEVICE_ADD_LINK_FLAGS_EGRESS_MAP_CKSUMV5,
                                   NULL, on_done, &w);
    guint mux_id = 0;
    g_autofree gchar *ifname =
            qmi_device_add_link_with_flags_finish(dev, wait_result(&w), &mux_id, &error);
    if (!ifname) fail("add link", error);

    if (wds4) {
        g_autoptr(QmiMessageWdsSetIpFamilyInput) fin = qmi_message_wds_set_ip_family_input_new();
        qmi_message_wds_set_ip_family_input_set_preference(fin, QMI_WDS_IP_FAMILY_IPV4, NULL);
        qmi_client_wds_set_ip_family(QMI_CLIENT_WDS(wds4), fin, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsSetIpFamilyOutput) fout =
                qmi_client_wds_set_ip_family_finish(QMI_CLIENT_WDS(wds4), wait_result(&w), &error);
        if (!fout || !qmi_message_wds_set_ip_family_output_get_result(fout, &error))
            g_clear_error(&error);

        g_autoptr(QmiMessageWdsBindMuxDataPortInput) in = qmi_message_wds_bind_mux_data_port_input_new();
        qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(in, ep_type, ep_index, NULL);
        qmi_message_wds_bind_mux_data_port_input_set_mux_id(in, mux_id, NULL);
        qmi_client_wds_bind_mux_data_port(QMI_CLIENT_WDS(wds4), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsBindMuxDataPortOutput) out =
                qmi_client_wds_bind_mux_data_port_finish(QMI_CLIENT_WDS(wds4), wait_result(&w), &error);
        if (!out || !qmi_message_wds_bind_mux_data_port_output_get_result(out, &error))
            fail("bind mux data port v4", error);
    }
    if (wds6) {
        g_autoptr(QmiMessageWdsSetIpFamilyInput) fin = qmi_message_wds_set_ip_family_input_new();
        qmi_message_wds_set_ip_family_input_set_preference(fin, QMI_WDS_IP_FAMILY_IPV6, NULL);
        qmi_client_wds_set_ip_family(QMI_CLIENT_WDS(wds6), fin, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsSetIpFamilyOutput) fout =
                qmi_client_wds_set_ip_family_finish(QMI_CLIENT_WDS(wds6), wait_result(&w), &error);
        if (!fout || !qmi_message_wds_set_ip_family_output_get_result(fout, &error))
            g_clear_error(&error);

        g_autoptr(QmiMessageWdsBindMuxDataPortInput) in = qmi_message_wds_bind_mux_data_port_input_new();
        qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(in, ep_type, ep_index, NULL);
        qmi_message_wds_bind_mux_data_port_input_set_mux_id(in, mux_id, NULL);
        qmi_client_wds_bind_mux_data_port(QMI_CLIENT_WDS(wds6), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsBindMuxDataPortOutput) out =
                qmi_client_wds_bind_mux_data_port_finish(QMI_CLIENT_WDS(wds6), wait_result(&w), &error);
        if (!out || !qmi_message_wds_bind_mux_data_port_output_get_result(out, &error))
            fail("bind mux data port v6", error);
    }

    /*
     * Bring up IPv6 before IPv4 on dual-stack carriers (such as Jio 5G SA / VoLTE):
     * starting IPv4 first on an IPv6-only/464XLAT bearer can trigger modem CLAT
     * state that blocks a subsequent IPv6 StartNetwork on the same mux_id.
     */
    guint32 handle4 = 0, handle6 = 0;
    if (wds6) {
        g_autoptr(QmiMessageWdsStartNetworkInput) in = qmi_message_wds_start_network_input_new();
        if (apn[0]) qmi_message_wds_start_network_input_set_apn(in, apn, NULL);
        qmi_message_wds_start_network_input_set_ip_family_preference(in, QMI_WDS_IP_FAMILY_IPV6, NULL);
        qmi_client_wds_start_network(QMI_CLIENT_WDS(wds6), in, 60, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsStartNetworkOutput) out =
                qmi_client_wds_start_network_finish(QMI_CLIENT_WDS(wds6), wait_result(&w), &error);
        if (!out || !qmi_message_wds_start_network_output_get_result(out, &error)) {
            if (!wds4) {
                QmiWdsCallEndReason reason;
                if (out && qmi_message_wds_start_network_output_get_call_end_reason(out, &reason, NULL))
                    printf("end_reason=%s\n", qmi_wds_call_end_reason_get_string(reason));
                qmi_device_delete_link(dev, ifname, mux_id, NULL, on_done, &w);
                qmi_device_delete_link_finish(dev, wait_result(&w), NULL);
                fail("start network v6", error);
            }
            g_clear_error(&error);
        } else {
            qmi_message_wds_start_network_output_get_packet_data_handle(out, &handle6, NULL);
        }
    }
    if (wds4) {
        g_autoptr(QmiMessageWdsStartNetworkInput) in = qmi_message_wds_start_network_input_new();
        if (apn[0]) qmi_message_wds_start_network_input_set_apn(in, apn, NULL);
        qmi_message_wds_start_network_input_set_ip_family_preference(in, QMI_WDS_IP_FAMILY_IPV4, NULL);
        qmi_client_wds_start_network(QMI_CLIENT_WDS(wds4), in, 60, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsStartNetworkOutput) out =
                qmi_client_wds_start_network_finish(QMI_CLIENT_WDS(wds4), wait_result(&w), &error);
        if (!out || !qmi_message_wds_start_network_output_get_result(out, &error)) {
            if (!handle6) {
                QmiWdsCallEndReason reason;
                if (out && qmi_message_wds_start_network_output_get_call_end_reason(out, &reason, NULL))
                    printf("end_reason=%s\n", qmi_wds_call_end_reason_get_string(reason));
                qmi_device_delete_link(dev, ifname, mux_id, NULL, on_done, &w);
                qmi_device_delete_link_finish(dev, wait_result(&w), NULL);
                fail("start network v4", error);
            }
            g_clear_error(&error);
        } else {
            qmi_message_wds_start_network_output_get_packet_data_handle(out, &handle4, NULL);
        }
    }

    printf("ifname=%s\n", ifname);
    guint32 mtu = 0;

    if (handle4) {
        g_autoptr(QmiMessageWdsGetCurrentSettingsInput) in = qmi_message_wds_get_current_settings_input_new();
        qmi_message_wds_get_current_settings_input_set_requested_settings(
                in,
                QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS |
                QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO |
                QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS |
                QMI_WDS_REQUESTED_SETTINGS_MTU |
                QMI_WDS_REQUESTED_SETTINGS_PCSCF_SERVER_ADDRESS_LIST |
                QMI_WDS_REQUESTED_SETTINGS_PCSCF_DOMAIN_NAME_LIST |
                QMI_WDS_REQUESTED_SETTINGS_IP_FAMILY,
                NULL);
        qmi_client_wds_get_current_settings(QMI_CLIENT_WDS(wds4), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsGetCurrentSettingsOutput) out =
                qmi_client_wds_get_current_settings_finish(QMI_CLIENT_WDS(wds4), wait_result(&w), &error);
        if (out && qmi_message_wds_get_current_settings_output_get_result(out, &error)) {
            guint32 addr = 0, mask = 0xffffffff, gw = 0, dns = 0, m4 = 0;
            if (qmi_message_wds_get_current_settings_output_get_ipv4_address(out, &addr, NULL)) {
                qmi_message_wds_get_current_settings_output_get_ipv4_gateway_subnet_mask(out, &mask, NULL);
                struct in_addr in4 = {.s_addr = htonl(addr)};
                printf("address=%s/%d\n", inet_ntoa(in4), __builtin_popcount(mask));
                configure_ipv4(ifname, addr, mask);
            }
            if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(out, &gw, NULL))
                print_ipv4("gateway", gw);
            if (qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(out, &dns, NULL))
                print_ipv4("dns", dns);
            if (qmi_message_wds_get_current_settings_output_get_secondary_ipv4_dns_address(out, &dns, NULL))
                print_ipv4("dns", dns);
            if (qmi_message_wds_get_current_settings_output_get_mtu(out, &m4, NULL) && m4 > mtu)
                mtu = m4;
            GArray *pcscf_list = NULL;
            if (qmi_message_wds_get_current_settings_output_get_pcscf_server_address_list(out, &pcscf_list, NULL) && pcscf_list) {
                for (guint i = 0; i < pcscf_list->len; i++) {
                    guint32 pcscf_addr = g_array_index(pcscf_list, guint32, i);
                    struct in_addr in_p = {.s_addr = htonl(pcscf_addr)};
                    printf("pcscf=%s\n", inet_ntoa(in_p));
                }
            }
        } else {
            g_clear_error(&error);
        }
    }

    if (handle6) {
        g_autoptr(QmiMessageWdsGetCurrentSettingsInput) in = qmi_message_wds_get_current_settings_input_new();
        qmi_message_wds_get_current_settings_input_set_requested_settings(
                in,
                QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS |
                QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO |
                QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS |
                QMI_WDS_REQUESTED_SETTINGS_MTU |
                QMI_WDS_REQUESTED_SETTINGS_PCSCF_SERVER_ADDRESS_LIST |
                QMI_WDS_REQUESTED_SETTINGS_PCSCF_DOMAIN_NAME_LIST |
                QMI_WDS_REQUESTED_SETTINGS_IP_FAMILY,
                NULL);
        qmi_client_wds_get_current_settings(QMI_CLIENT_WDS(wds6), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsGetCurrentSettingsOutput) out =
                qmi_client_wds_get_current_settings_finish(QMI_CLIENT_WDS(wds6), wait_result(&w), &error);
        if (out && qmi_message_wds_get_current_settings_output_get_result(out, &error)) {
            GArray *array6 = NULL;
            guint8 prefix6 = 0;
            guint32 m6 = 0;
            char buf6[INET6_ADDRSTRLEN];
            struct in6_addr in6_addr_val;

            if (qmi_message_wds_get_current_settings_output_get_ipv6_address(out, &array6, &prefix6, NULL) && array6) {
                memset(&in6_addr_val, 0, sizeof(in6_addr_val));
                for (guint i = 0; i < array6->len && i < 8; i++)
                    ((guint16 *)in6_addr_val.s6_addr)[i] = GUINT16_TO_BE(g_array_index(array6, guint16, i));
                inet_ntop(AF_INET6, &in6_addr_val, buf6, sizeof(buf6));
                printf("ipv6_address=%s/%u\n", buf6, prefix6);
                configure_ipv6(ifname, &in6_addr_val, prefix6);
            }
            if (qmi_message_wds_get_current_settings_output_get_ipv6_gateway_address(out, &array6, &prefix6, NULL) && array6) {
                memset(&in6_addr_val, 0, sizeof(in6_addr_val));
                for (guint i = 0; i < array6->len && i < 8; i++)
                    ((guint16 *)in6_addr_val.s6_addr)[i] = GUINT16_TO_BE(g_array_index(array6, guint16, i));
                inet_ntop(AF_INET6, &in6_addr_val, buf6, sizeof(buf6));
                printf("gateway=%s\n", buf6);
            }
            if (qmi_message_wds_get_current_settings_output_get_ipv6_primary_dns_address(out, &array6, NULL) && array6) {
                memset(&in6_addr_val, 0, sizeof(in6_addr_val));
                for (guint i = 0; i < array6->len && i < 8; i++)
                    ((guint16 *)in6_addr_val.s6_addr)[i] = GUINT16_TO_BE(g_array_index(array6, guint16, i));
                inet_ntop(AF_INET6, &in6_addr_val, buf6, sizeof(buf6));
                printf("dns=%s\n", buf6);
            }
            if (qmi_message_wds_get_current_settings_output_get_ipv6_secondary_dns_address(out, &array6, NULL) && array6) {
                memset(&in6_addr_val, 0, sizeof(in6_addr_val));
                for (guint i = 0; i < array6->len && i < 8; i++)
                    ((guint16 *)in6_addr_val.s6_addr)[i] = GUINT16_TO_BE(g_array_index(array6, guint16, i));
                inet_ntop(AF_INET6, &in6_addr_val, buf6, sizeof(buf6));
                printf("dns=%s\n", buf6);
            }
            if (qmi_message_wds_get_current_settings_output_get_mtu(out, &m6, NULL) && m6 > mtu)
                mtu = m6;
            GArray *pcscf_domains = NULL;
            if (qmi_message_wds_get_current_settings_output_get_pcscf_domain_name_list(out, &pcscf_domains, NULL) && pcscf_domains) {
                for (guint i = 0; i < pcscf_domains->len; i++) {
                    const gchar *domain = g_array_index(pcscf_domains, const gchar *, i);
                    if (domain && domain[0]) printf("pcscf=%s\n", domain);
                }
            }
            print_ipv6_pcscf(dev, wds6, &w);
        } else {
            g_clear_error(&error);
        }
    }

    if (handle4 && handle6) {
        printf("protocol=IPV4V6\n");
    } else if (handle6) {
        printf("protocol=IPV6\n");
    } else if (handle4) {
        printf("protocol=IP\n");
    }
    if (mtu) {
        printf("mtu=%u\n", mtu);
        set_iface_mtu(ifname, mtu);
    }
    set_iface_up(ifname);

    printf("ready\n");
    fflush(stdout);

    g_unix_signal_add(SIGTERM, on_sigterm, w.loop);
    g_unix_signal_add(SIGINT, on_sigterm, w.loop);
    if (wds4) g_signal_connect(wds4, "packet-service-status", G_CALLBACK(on_packet_service_status), w.loop);
    if (wds6) g_signal_connect(wds6, "packet-service-status", G_CALLBACK(on_packet_service_status), w.loop);
    g_main_loop_run(w.loop);

    if (!s_disconnected) {
        if (handle4 && wds4) {
            g_autoptr(QmiMessageWdsStopNetworkInput) in = qmi_message_wds_stop_network_input_new();
            qmi_message_wds_stop_network_input_set_packet_data_handle(in, handle4, NULL);
            qmi_client_wds_stop_network(QMI_CLIENT_WDS(wds4), in, 10, NULL, on_done, &w);
            g_autoptr(QmiMessageWdsStopNetworkOutput) out =
                    qmi_client_wds_stop_network_finish(QMI_CLIENT_WDS(wds4), wait_result(&w), NULL);
        }
        if (handle6 && wds6) {
            g_autoptr(QmiMessageWdsStopNetworkInput) in = qmi_message_wds_stop_network_input_new();
            qmi_message_wds_stop_network_input_set_packet_data_handle(in, handle6, NULL);
            qmi_client_wds_stop_network(QMI_CLIENT_WDS(wds6), in, 10, NULL, on_done, &w);
            g_autoptr(QmiMessageWdsStopNetworkOutput) out =
                    qmi_client_wds_stop_network_finish(QMI_CLIENT_WDS(wds6), wait_result(&w), NULL);
        }
    }
    qmi_device_delete_link(dev, ifname, mux_id, NULL, on_done, &w);
    qmi_device_delete_link_finish(dev, wait_result(&w), NULL);
    release_client(dev, wds6, &w);
    release_client(dev, wds4, &w);
    release_client(dev, wda, &w);
    return 0;
}
