#include "mqtt_boot.h"

#include <string.h>
#include <stdio.h>

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "main.h"
#include "ethernet_ota.h"

/* ============================================================
   EXTERNALS FROM YOUR BOOTLOADER
   ============================================================ */
extern struct netif gnetif;
extern volatile bool ota_upload_in_progress;
extern volatile bool ota_upload_done;

/* ============================================================
   MQTT CONFIGURATION
   ============================================================ */
#define MQTT_BROKER_IP       "192.168.1.10"
#define MQTT_BROKER_PORT     1883

#define MQTT_CLIENT_ID       "stm32h7_boot"

/* Topics */
#define MQTT_TOPIC_CMD       "boot/stm32h7_001/cmd"
#define MQTT_TOPIC_STATUS    "boot/stm32h7_001/status"
#define MQTT_TOPIC_VOLTAGE   "boot/stm32h7_001/voltage"
#define MQTT_TOPIC_LOG       "boot/stm32h7_001/log"

/* ============================================================
   MQTT STATE MACHINE
   ============================================================ */
typedef enum {
    MQTT_STATE_IDLE = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_SUBSCRIBED,
    MQTT_STATE_ERROR
} mqtt_state_t;

static mqtt_state_t mqtt_state = MQTT_STATE_IDLE;

/* ============================================================
   MQTT INTERNALS
   ============================================================ */
static mqtt_client_t *mqtt_client = NULL;
static struct mqtt_connect_client_info_t mqtt_ci;
static ip_addr_t broker_ip;

/* ============================================================
   FORWARD DECLARATIONS
   ============================================================ */
static void mqtt_connection_cb(mqtt_client_t *client,
                               void *arg,
                               mqtt_connection_status_t status);

static void mqtt_subscribe_cb(void *arg, err_t result);

static void mqtt_incoming_publish_cb(void *arg,
                                     const char *topic,
                                     u32_t tot_len);

static void mqtt_incoming_data_cb(void *arg,
                                  const u8_t *data,
                                  u16_t len,
                                  u8_t flags);

/* ============================================================
   PUBLIC API
   ============================================================ */

void mqtt_boot_init(void)
{
    mqtt_client = mqtt_client_new();
    if (mqtt_client == NULL) {
        mqtt_state = MQTT_STATE_ERROR;
        return;
    }

    memset(&mqtt_ci, 0, sizeof(mqtt_ci));
    mqtt_ci.client_id = MQTT_CLIENT_ID;
    mqtt_ci.keep_alive = 30;

    ipaddr_aton(MQTT_BROKER_IP, &broker_ip);

    mqtt_state = MQTT_STATE_IDLE;
}

bool mqtt_is_connected(void)
{
    return (mqtt_state >= MQTT_STATE_CONNECTED);
}

void mqtt_boot_periodic(void)
{
    if (!netif_is_link_up(&gnetif)) {
        return;
    }

    switch (mqtt_state) {

    case MQTT_STATE_IDLE:
        mqtt_state = MQTT_STATE_CONNECTING;
        mqtt_client_connect(mqtt_client,
                            &broker_ip,
                            MQTT_BROKER_PORT,
                            mqtt_connection_cb,
                            NULL,
                            &mqtt_ci);
        break;

    case MQTT_STATE_ERROR:
        /* Retry after error */
        mqtt_state = MQTT_STATE_IDLE;
        break;

    default:
        break;
    }
}

/* ============================================================
   MQTT CALLBACKS
   ============================================================ */

static void mqtt_connection_cb(mqtt_client_t *client,
                               void *arg,
                               mqtt_connection_status_t status)
{
    if (status != MQTT_CONNECT_ACCEPTED) {
        mqtt_state = MQTT_STATE_ERROR;
        return;
    }

    mqtt_state = MQTT_STATE_CONNECTED;

    /* Register RX callbacks */
    mqtt_set_inpub_callback(client,
                            mqtt_incoming_publish_cb,
                            mqtt_incoming_data_cb,
                            NULL);

    /* Subscribe to command topic */
    mqtt_subscribe(client,
                   MQTT_TOPIC_CMD,
                   0,
                   mqtt_subscribe_cb,
                   NULL);

    mqtt_publish_status("online");
}

static void mqtt_subscribe_cb(void *arg, err_t result)
{
    if (result == ERR_OK) {
        mqtt_state = MQTT_STATE_SUBSCRIBED;
        mqtt_publish_log("Subscribed to command topic");
    }
}

static void mqtt_incoming_publish_cb(void *arg,
                                     const char *topic,
                                     u32_t tot_len)
{
    /* Topic received — nothing needed here for now */
    (void)arg;
    (void)topic;
    (void)tot_len;
}

static void mqtt_incoming_data_cb(void *arg,
                                  const u8_t *data,
                                  u16_t len,
                                  u8_t flags)
{
    (void)arg;
    (void)flags;

    char payload[128];

    if (len >= sizeof(payload)) {
        return;
    }

    memcpy(payload, data, len);
    payload[len] = '\0';

    /* ================= COMMAND HANDLING ================= */

    if (strstr(payload, "start_ota")) {
        mqtt_publish_log("MQTT: OTA requested");
        ota_upload_in_progress = true;
        ethernet_ota_init();
    }

    else if (strstr(payload, "reboot")) {
        mqtt_publish_log("MQTT: reboot requested");
        HAL_Delay(100);
        NVIC_SystemReset();
    }

    else if (strstr(payload, "status")) {
        mqtt_publish_status("alive");
    }
}

/* ============================================================
   PUBLISH HELPERS
   ============================================================ */

void mqtt_publish_status(const char *status)
{
    if (!mqtt_is_connected()) return;

    mqtt_publish(mqtt_client,
                 MQTT_TOPIC_STATUS,
                 status,
                 strlen(status),
                 0,
                 0,
                 NULL,
                 NULL);
}

void mqtt_publish_voltage(float voltage)
{
    if (!mqtt_is_connected()) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", voltage);

    mqtt_publish(mqtt_client,
                 MQTT_TOPIC_VOLTAGE,
                 buf,
                 strlen(buf),
                 0,
                 0,
                 NULL,
                 NULL);
}

void mqtt_publish_log(const char *msg)
{
    if (!mqtt_is_connected()) return;

    mqtt_publish(mqtt_client,
                 MQTT_TOPIC_LOG,
                 msg,
                 strlen(msg),
                 0,
                 0,
                 NULL,
                 NULL);
}
