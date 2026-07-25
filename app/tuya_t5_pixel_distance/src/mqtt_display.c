/**
 * @file mqtt_display.c
 * @brief WiFi + MQTT subscriber that maps Go2 robot dog actions to display commands
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "mqtt_display.h"
#include "wifi_credentials.h"

#include "tal_api.h"
#include "tal_wifi.h"
#include "mqtt_client_interface.h"

#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/
/* WiFi SSID/password live in the git-ignored wifi_credentials.h. */
#define MQTT_DISPLAY_BROKER_HOST   "192.168.5.10"
#define MQTT_DISPLAY_BROKER_PORT   1883
#define MQTT_DISPLAY_TOPIC         "go2/B42D1000Q5SAKA07/display"
#define MQTT_DISPLAY_CLIENT_ID     "t5-pixel-display"
#define MQTT_DISPLAY_KEEPALIVE_S   60
#define MQTT_DISPLAY_TIMEOUT_MS    3000
#define MQTT_DISPLAY_RETRY_MS      3000
#define MQTT_DISPLAY_WIFI_RETRY_MS 10000

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    const char        *name;
    MQTT_DISPLAY_CMD_E cmd;
} mqtt_cmd_map_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static THREAD_HANDLE                 g_mqtt_thread    = NULL;
static volatile bool                 g_wifi_connected = false;
static volatile bool                 g_mqtt_connected = false;
static volatile MQTT_DISPLAY_CMD_E   g_display_cmd    = MQTT_DISPLAY_CMD_IDLE;
static char                          g_ip_str[16]     = {0};

static const mqtt_cmd_map_t g_cmd_map[] = {
    {"forward", MQTT_DISPLAY_CMD_FORWARD}, {"backward", MQTT_DISPLAY_CMD_BACKWARD},
    {"turn_left", MQTT_DISPLAY_CMD_TURN_LEFT}, {"turn_right", MQTT_DISPLAY_CMD_TURN_RIGHT},
    {"heart", MQTT_DISPLAY_CMD_HEART}, {"smile", MQTT_DISPLAY_CMD_SMILE},
    {"idle", MQTT_DISPLAY_CMD_IDLE},
};

/***********************************************************
***********************function define**********************
***********************************************************/
static void mqtt_display_wifi_event_cb(WF_EVENT_E event, void *arg)
{
    switch (event) {
    case WFE_CONNECTED: {
        NW_IP_S ip_info;

        memset(&ip_info, 0, sizeof(ip_info));
        g_wifi_connected = true;
        if (tal_wifi_get_ip(WF_STATION, &ip_info) == OPRT_OK) {
            strncpy(g_ip_str, ip_info.ip, sizeof(g_ip_str) - 1);
            PR_NOTICE("WiFi connected to %s, ip=%s", MQTT_DISPLAY_WIFI_SSID, ip_info.ip);
        }
        break;
    }
    case WFE_CONNECT_FAILED:
        PR_WARN("WiFi connect failed");
        g_wifi_connected = false;
        break;
    case WFE_DISCONNECTED:
        PR_WARN("WiFi disconnected");
        g_wifi_connected = false;
        break;
    default:
        break;
    }
}

static void mqtt_display_connected_cb(void *client, void *userdata)
{
    uint16_t msgid;

    g_mqtt_connected = true;
    msgid = mqtt_client_subscribe(client, MQTT_DISPLAY_TOPIC, MQTT_QOS_1);
    if (msgid == 0) {
        PR_ERR("MQTT subscribe %s failed", MQTT_DISPLAY_TOPIC);
        return;
    }
    PR_NOTICE("MQTT connected, subscribing %s id=%u", MQTT_DISPLAY_TOPIC, msgid);
}

static void mqtt_display_disconnected_cb(void *client, void *userdata)
{
    g_mqtt_connected = false;
    PR_WARN("MQTT disconnected from %s", MQTT_DISPLAY_BROKER_HOST);
}

static void mqtt_display_message_cb(void *client, uint16_t msgid, const mqtt_client_message_t *msg, void *userdata)
{
    size_t length = msg->length;

    /* Trim trailing NUL/CR/LF/space so `mosquitto_pub -m "forward"` variants all match. */
    while (length > 0) {
        uint8_t tail = msg->payload[length - 1];
        if (tail != '\0' && tail != '\r' && tail != '\n' && tail != ' ') {
            break;
        }
        length--;
    }

    for (uint32_t i = 0; i < sizeof(g_cmd_map) / sizeof(g_cmd_map[0]); i++) {
        if (strlen(g_cmd_map[i].name) == length && memcmp(g_cmd_map[i].name, msg->payload, length) == 0) {
            g_display_cmd = g_cmd_map[i].cmd;
            PR_NOTICE("MQTT display command: %s", g_cmd_map[i].name);
            return;
        }
    }
    PR_WARN("MQTT unknown payload (len=%u), ignored", (uint32_t)msg->length);
}

static void mqtt_display_thread(void *arg)
{
    void                      *client = NULL;
    uint32_t                   last_wifi_retry_ms;
    const mqtt_client_config_t mqtt_config = {
        .cacert          = NULL,
        .cacert_len      = 0,
        .host            = MQTT_DISPLAY_BROKER_HOST,
        .port            = MQTT_DISPLAY_BROKER_PORT,
        .keepalive       = MQTT_DISPLAY_KEEPALIVE_S,
        .timeout_ms      = MQTT_DISPLAY_TIMEOUT_MS,
        .clientid        = MQTT_DISPLAY_CLIENT_ID,
        .username        = "",
        .password        = "",
        .on_connected    = mqtt_display_connected_cb,
        .on_disconnected = mqtt_display_disconnected_cb,
        .on_message      = mqtt_display_message_cb,
        .userdata        = NULL,
    };

    if (tal_wifi_init(mqtt_display_wifi_event_cb) != OPRT_OK) {
        PR_ERR("tal_wifi_init failed");
        goto __EXIT;
    }
    tal_wifi_set_work_mode(WWM_STATION);
    /* Beken WiFi power save drops the STA link a few seconds after DHCP; keep the radio awake. */
    tal_wifi_lp_disable();
    PR_NOTICE("Connecting WiFi ssid=%s", MQTT_DISPLAY_WIFI_SSID);
    tal_wifi_station_connect((int8_t *)MQTT_DISPLAY_WIFI_SSID, (int8_t *)MQTT_DISPLAY_WIFI_PSWD);
    last_wifi_retry_ms = (uint32_t)tal_system_get_millisecond();

    client = mqtt_client_new();
    if (client == NULL) {
        PR_ERR("mqtt_client_new failed");
        goto __EXIT;
    }
    if (mqtt_client_init(client, &mqtt_config) != MQTT_STATUS_SUCCESS) {
        PR_ERR("mqtt_client_init failed");
        goto __EXIT;
    }

    while (1) {
        if (!g_wifi_connected) {
            uint32_t now_ms = (uint32_t)tal_system_get_millisecond();
            if (now_ms - last_wifi_retry_ms >= MQTT_DISPLAY_WIFI_RETRY_MS) {
                PR_NOTICE("Retry WiFi connect ssid=%s", MQTT_DISPLAY_WIFI_SSID);
                tal_wifi_station_connect((int8_t *)MQTT_DISPLAY_WIFI_SSID, (int8_t *)MQTT_DISPLAY_WIFI_PSWD);
                last_wifi_retry_ms = now_ms;
            }
            tal_system_sleep(500);
            continue;
        }

        mqtt_client_status_t status = mqtt_client_connect(client);
        if (status != MQTT_STATUS_SUCCESS) {
            PR_WARN("MQTT connect %s:%d failed: %d", MQTT_DISPLAY_BROKER_HOST, MQTT_DISPLAY_BROKER_PORT, status);
            tal_system_sleep(MQTT_DISPLAY_RETRY_MS);
            continue;
        }

        while (g_wifi_connected) {
            if (mqtt_client_yield(client) != MQTT_STATUS_SUCCESS) {
                break;
            }
        }
        g_mqtt_connected = false;
        tal_system_sleep(MQTT_DISPLAY_RETRY_MS);
    }

__EXIT:
    if (client != NULL) {
        mqtt_client_free(client);
    }
    tal_thread_delete(g_mqtt_thread);
    g_mqtt_thread = NULL;
}

OPERATE_RET mqtt_display_start(void)
{
    THREAD_CFG_T thread_config = {0};

    if (g_mqtt_thread != NULL) {
        return OPRT_OK;
    }
    thread_config.stackDepth = 1024 * 4;
    thread_config.priority   = THREAD_PRIO_2;
    thread_config.thrdname   = "mqtt_display";
    return tal_thread_create_and_start(&g_mqtt_thread, NULL, NULL, mqtt_display_thread, NULL, &thread_config);
}

MQTT_DISPLAY_CMD_E mqtt_display_get_cmd(void)
{
    return g_display_cmd;
}

void mqtt_display_get_status(MQTT_DISPLAY_STATUS_T *status)
{
    if (status == NULL) {
        return;
    }
    status->wifi_connected = g_wifi_connected;
    status->mqtt_connected = g_mqtt_connected;
    strncpy(status->ip, g_wifi_connected ? g_ip_str : "-", sizeof(status->ip) - 1);
    status->ip[sizeof(status->ip) - 1] = '\0';
}
