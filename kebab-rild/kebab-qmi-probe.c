/*
 * kebab-qmi-probe — M0 transport proof for the kebab mainline stack.
 *
 * Opens the QRTR bus, prints the modem node's published QMI services, then
 * creates a libqmi QmiDevice over that node and allocates one client ID.
 * Success means: libqmi/glib link correctly, the QRTR transport works, and
 * QMI CTL accepts a client allocation on the SDX55 node.
 */

#include <gio/gio.h>
#include <glib.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>

#define LOOKUP_TIMEOUT_MS 5000
#define PROBE_DEADLINE_S 20

static GMainLoop *loop;
static QrtrBus *bus;
static QmiDevice *device;
static int exit_code = 1;

static void finish(int code)
{
    exit_code = code;
    g_main_loop_quit(loop);
}

static gboolean deadline_cb(gpointer data)
{
    (void)data;
    g_printerr("probe: deadline reached\n");
    finish(1);
    return G_SOURCE_REMOVE;
}

static const char *svc_name(guint32 service)
{
    switch (service) {
    case QMI_SERVICE_CTL: return "CTL";
    case QMI_SERVICE_WDS: return "WDS";
    case QMI_SERVICE_DMS: return "DMS";
    case QMI_SERVICE_NAS: return "NAS";
    case QMI_SERVICE_WMS: return "WMS";
    case QMI_SERVICE_VOICE: return "VOICE";
    case QMI_SERVICE_UIM: return "UIM";
    case QMI_SERVICE_IMSA: return "IMSA";
    case QMI_SERVICE_DSD: return "DSD";
    default: return "OTHER";
    }
}

static void client_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QmiClient *client;

    (void)data;
    client = qmi_device_allocate_client_finish(QMI_DEVICE(src), res, &error);
    if (!client) {
        g_printerr("probe: allocate DMS client failed: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        finish(1);
        return;
    }
    g_print("probe: allocated %s client cid=%u\n",
            svc_name(qmi_client_get_service(client)), qmi_client_get_cid(client));
    g_object_unref(client);
    g_print("probe: OK\n");
    finish(0);
}

static void device_opened(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;

    (void)data;
    if (!qmi_device_open_finish(QMI_DEVICE(src), res, &error)) {
        g_printerr("probe: qmi_device_open failed: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        finish(1);
        return;
    }
    g_print("probe: device open, allocating DMS client\n");
    qmi_device_allocate_client(QMI_DEVICE(src), QMI_SERVICE_DMS, QMI_CID_NONE,
                               10, NULL, client_ready, NULL);
}

static void device_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;

    (void)src;
    (void)data;
    device = qmi_device_new_from_node_finish(res, &error);
    if (!device) {
        g_printerr("probe: qmi_device_new_from_node failed: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        finish(1);
        return;
    }
    g_print("probe: QmiDevice created over QRTR\n");
    qmi_device_open(device, QMI_DEVICE_OPEN_FLAGS_VERSION_INFO, 10, NULL,
                    device_opened, NULL);
}

static void bus_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    QrtrNode *modem = NULL;
    GList *nodes;

    (void)src;
    (void)data;
    bus = qrtr_bus_new_finish(res, &error);
    if (!bus) {
        g_printerr("probe: qrtr_bus_new failed: %s\n",
                   error ? error->message : "unknown");
        g_clear_error(&error);
        finish(1);
        return;
    }

    for (nodes = qrtr_bus_peek_nodes(bus); nodes; nodes = nodes->next) {
        QrtrNode *node = QRTR_NODE(nodes->data);
        GList *si;
        gboolean has_voice = FALSE;

        g_print("probe: node %u:", qrtr_node_get_id(node));
        for (si = qrtr_node_peek_service_info_list(node); si; si = si->next) {
            QrtrNodeServiceInfo *info = (QrtrNodeServiceInfo *)si->data;
            guint32 svc = qrtr_node_service_info_get_service(info);

            g_print(" %s(0x%02x) port=%u ver=%u inst=%u",
                    svc_name(svc), svc, qrtr_node_service_info_get_port(info),
                    qrtr_node_service_info_get_version(info),
                    qrtr_node_service_info_get_instance(info));
            if (svc == QMI_SERVICE_VOICE)
                has_voice = TRUE;
        }
        g_print("\n");

        if (!modem && has_voice)
            modem = node;
        else if (!modem && qrtr_node_get_id(node) == 3)
            modem = node;
    }

    if (!modem) {
        g_printerr("probe: no modem node with QMI services found\n");
        finish(1);
        return;
    }
    g_print("probe: using modem node %u\n", qrtr_node_get_id(modem));
    qmi_device_new_from_node(modem, NULL, device_ready, NULL);
}

int main(void)
{
    GSource *deadline;

    loop = g_main_loop_new(NULL, FALSE);
    deadline = g_timeout_source_new_seconds(PROBE_DEADLINE_S);
    g_source_set_callback(deadline, deadline_cb, NULL, NULL);
    g_source_attach(deadline, NULL);

    qrtr_bus_new(LOOKUP_TIMEOUT_MS, NULL, bus_ready, NULL);
    g_main_loop_run(loop);

    g_source_destroy(deadline);
    g_source_unref(deadline);
    if (device)
        g_object_unref(device);
    if (bus)
        g_object_unref(bus);
    g_main_loop_unref(loop);
    return exit_code;
}
