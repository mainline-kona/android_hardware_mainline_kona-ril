/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Brings up one mobile data session on the modem behind IPA and holds it until SIGTERM.
 *
 * Usage: pong-dataconn <apn> <ipv4|ipv6|ipv4v6>
 * Prints key=value lines (ifname, address, gateway, dns, mtu) and then "ready" once the
 * session is up, or "error=<reason>" and exits non-zero. Prints "disconnected" and exits
 * when the modem drops the session.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glib-unix.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define IPA_IFACE "rmnet_ipa0"
#define LINK_PREFIX "rmnet_data"
#define MUX_ID 1

typedef struct {
    GMainLoop *loop;
    GAsyncResult *res;
} Wait;

static void on_done(GObject *source, GAsyncResult *res, gpointer data) {
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

static void configure_ipv4(const char *ifname, guint32 addr, guint32 mask, guint32 mtu) {
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
    if (mtu) {
        ifr.ifr_mtu = mtu;
        if (ioctl(fd, SIOCSIFMTU, &ifr) < 0) printf("warning=set mtu: %s\n", strerror(errno));
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
    QmiWdsConnectionStatus status;
    if (!qmi_indication_wds_packet_service_status_output_get_connection_status(ind, &status, NULL, NULL) ||
            status != QMI_WDS_CONNECTION_STATUS_DISCONNECTED)
        return;
    s_disconnected = TRUE;
    printf("disconnected\n");
    fflush(stdout);
    g_main_loop_quit(loop);
}

int main(int argc, char **argv) {
    GError *error = NULL;
    Wait w = {.loop = g_main_loop_new(NULL, FALSE)};
    const char *apn = argc > 1 ? argv[1] : "";
    const char *protocol = argc > 2 ? argv[2] : "ipv4";

    qrtr_bus_new(1000, NULL, on_done, &w);
    g_autoptr(QrtrBus) bus = qrtr_bus_new_finish(wait_result(&w), &error);
    if (!bus) fail("open qrtr bus", error);

    /* The modem's node only shows up once it has booted */
    qrtr_bus_wait_for_node(bus, 0, 30000, NULL, on_done, &w);
    g_autoptr(QrtrNode) node = qrtr_bus_wait_for_node_finish(bus, wait_result(&w), &error);
    if (!node) fail("find modem node", error);

    qmi_device_new_from_node(node, NULL, on_done, &w);
    g_autoptr(QmiDevice) dev = qmi_device_new_from_node_finish(wait_result(&w), &error);
    if (!dev) fail("open device", error);

    qmi_device_open(dev, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, on_done, &w);
    if (!qmi_device_open_finish(dev, wait_result(&w), &error)) fail("open device", error);

    QmiClient *dpm = allocate(dev, QMI_SERVICE_DPM, &w);
    QmiClient *wda = allocate(dev, QMI_SERVICE_WDA, &w);
    QmiClient *wds = allocate(dev, QMI_SERVICE_WDS, &w);

    /* The modem's TX endpoint feeds IPA's RX port and the other way round */
    guint tx_id = read_sysfs_uint("/sys/class/net/" IPA_IFACE "/device/modem/tx_endpoint_id");
    guint rx_id = read_sysfs_uint("/sys/class/net/" IPA_IFACE "/device/modem/rx_endpoint_id");
    if (tx_id && rx_id) {
        QmiMessageDpmOpenPortInputHardwareDataPortsElement port = {
            .endpoint_type = QMI_DATA_ENDPOINT_TYPE_EMBEDDED,
            .interface_number = 1,
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

    {
        g_autoptr(QmiMessageWdaSetDataFormatInput) in = qmi_message_wda_set_data_format_input_new();
        qmi_message_wda_set_data_format_input_set_link_layer_protocol(
                in, QMI_WDA_LINK_LAYER_PROTOCOL_RAW_IP, NULL);
        qmi_message_wda_set_data_format_input_set_uplink_data_aggregation_protocol(
                in, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5, NULL);
        qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_protocol(
                in, QMI_WDA_DATA_AGGREGATION_PROTOCOL_QMAPV5, NULL);
        qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_size(in, 32768, NULL);
        qmi_message_wda_set_data_format_input_set_downlink_data_aggregation_max_datagrams(in, 32, NULL);
        qmi_message_wda_set_data_format_input_set_endpoint_info(
                in, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
        qmi_client_wda_set_data_format(QMI_CLIENT_WDA(wda), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdaSetDataFormatOutput) out =
                qmi_client_wda_set_data_format_finish(QMI_CLIENT_WDA(wda), wait_result(&w), &error);
        if (!out || !qmi_message_wda_set_data_format_output_get_result(out, &error))
            fail("set data format", error);
    }

    set_iface_up(IPA_IFACE);
    qmi_device_delete_all_links(dev, IPA_IFACE, NULL, on_done, &w);
    qmi_device_delete_all_links_finish(dev, wait_result(&w), NULL);

    qmi_device_add_link_with_flags(dev, MUX_ID, IPA_IFACE, LINK_PREFIX,
                                   QMI_DEVICE_ADD_LINK_FLAGS_INGRESS_MAP_CKSUMV5 |
                                           QMI_DEVICE_ADD_LINK_FLAGS_EGRESS_MAP_CKSUMV5,
                                   NULL, on_done, &w);
    guint mux_id = 0;
    g_autofree gchar *ifname =
            qmi_device_add_link_with_flags_finish(dev, wait_result(&w), &mux_id, &error);
    if (!ifname) fail("add link", error);

    {
        g_autoptr(QmiMessageWdsBindMuxDataPortInput) in = qmi_message_wds_bind_mux_data_port_input_new();
        qmi_message_wds_bind_mux_data_port_input_set_endpoint_info(
                in, QMI_DATA_ENDPOINT_TYPE_EMBEDDED, 1, NULL);
        qmi_message_wds_bind_mux_data_port_input_set_mux_id(in, MUX_ID, NULL);
        qmi_client_wds_bind_mux_data_port(QMI_CLIENT_WDS(wds), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsBindMuxDataPortOutput) out =
                qmi_client_wds_bind_mux_data_port_finish(QMI_CLIENT_WDS(wds), wait_result(&w), &error);
        if (!out || !qmi_message_wds_bind_mux_data_port_output_get_result(out, &error))
            fail("bind mux data port", error);
    }

    guint32 handle = 0;
    {
        g_autoptr(QmiMessageWdsStartNetworkInput) in = qmi_message_wds_start_network_input_new();
        if (apn[0]) qmi_message_wds_start_network_input_set_apn(in, apn, NULL);
        qmi_message_wds_start_network_input_set_ip_family_preference(
                in, strcmp(protocol, "ipv6") == 0 ? QMI_WDS_IP_FAMILY_IPV6 : QMI_WDS_IP_FAMILY_IPV4, NULL);
        qmi_client_wds_start_network(QMI_CLIENT_WDS(wds), in, 60, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsStartNetworkOutput) out =
                qmi_client_wds_start_network_finish(QMI_CLIENT_WDS(wds), wait_result(&w), &error);
        if (!out || !qmi_message_wds_start_network_output_get_result(out, &error)) {
            QmiWdsCallEndReason reason;
            if (out && qmi_message_wds_start_network_output_get_call_end_reason(out, &reason, NULL))
                printf("end_reason=%s\n", qmi_wds_call_end_reason_get_string(reason));
            fail("start network", error);
        }
        qmi_message_wds_start_network_output_get_packet_data_handle(out, &handle, NULL);
    }

    {
        g_autoptr(QmiMessageWdsGetCurrentSettingsInput) in = qmi_message_wds_get_current_settings_input_new();
        qmi_message_wds_get_current_settings_input_set_requested_settings(
                in,
                QMI_WDS_REQUESTED_SETTINGS_IP_ADDRESS | QMI_WDS_REQUESTED_SETTINGS_GATEWAY_INFO |
                        QMI_WDS_REQUESTED_SETTINGS_DNS_ADDRESS | QMI_WDS_REQUESTED_SETTINGS_MTU,
                NULL);
        qmi_client_wds_get_current_settings(QMI_CLIENT_WDS(wds), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsGetCurrentSettingsOutput) out =
                qmi_client_wds_get_current_settings_finish(QMI_CLIENT_WDS(wds), wait_result(&w), &error);
        if (!out || !qmi_message_wds_get_current_settings_output_get_result(out, &error))
            fail("get current settings", error);

        guint32 addr, mask = 0xffffffff, gw, dns, mtu = 0;
        gboolean has_addr;
        printf("ifname=%s\n", ifname);
        has_addr = qmi_message_wds_get_current_settings_output_get_ipv4_address(out, &addr, NULL);
        if (has_addr) {
            qmi_message_wds_get_current_settings_output_get_ipv4_gateway_subnet_mask(out, &mask, NULL);
            struct in_addr in4 = {.s_addr = htonl(addr)};
            printf("address=%s/%d\n", inet_ntoa(in4), __builtin_popcount(mask));
        }
        if (qmi_message_wds_get_current_settings_output_get_ipv4_gateway_address(out, &gw, NULL))
            print_ipv4("gateway", gw);
        if (qmi_message_wds_get_current_settings_output_get_primary_ipv4_dns_address(out, &dns, NULL))
            print_ipv4("dns", dns);
        if (qmi_message_wds_get_current_settings_output_get_secondary_ipv4_dns_address(out, &dns, NULL))
            print_ipv4("dns", dns);
        if (qmi_message_wds_get_current_settings_output_get_mtu(out, &mtu, NULL))
            printf("mtu=%u\n", mtu);

        if (has_addr) configure_ipv4(ifname, addr, mask, mtu);
        set_iface_up(ifname);
    }

    printf("ready\n");
    fflush(stdout);

    g_unix_signal_add(SIGTERM, on_sigterm, w.loop);
    g_unix_signal_add(SIGINT, on_sigterm, w.loop);
    g_signal_connect(wds, "packet-service-status", G_CALLBACK(on_packet_service_status), w.loop);
    g_main_loop_run(w.loop);

    if (!s_disconnected) {
        g_autoptr(QmiMessageWdsStopNetworkInput) in = qmi_message_wds_stop_network_input_new();
        qmi_message_wds_stop_network_input_set_packet_data_handle(in, handle, NULL);
        qmi_client_wds_stop_network(QMI_CLIENT_WDS(wds), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageWdsStopNetworkOutput) out =
                qmi_client_wds_stop_network_finish(QMI_CLIENT_WDS(wds), wait_result(&w), NULL);
    }
    qmi_device_delete_link(dev, ifname, mux_id, NULL, on_done, &w);
    qmi_device_delete_link_finish(dev, wait_result(&w), NULL);
    return 0;
}
