/**
 * @file mqtt_display.h
 * @brief MQTT display command channel for the Go2 robot dog matrix screen
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef MQTT_DISPLAY_H
#define MQTT_DISPLAY_H

#include "tuya_cloud_types.h"

#include <stdbool.h>

typedef enum {
    MQTT_DISPLAY_CMD_IDLE = 0,
    MQTT_DISPLAY_CMD_FORWARD,
    MQTT_DISPLAY_CMD_BACKWARD,
    MQTT_DISPLAY_CMD_TURN_LEFT,
    MQTT_DISPLAY_CMD_TURN_RIGHT,
    MQTT_DISPLAY_CMD_HEART,
    MQTT_DISPLAY_CMD_SMILE,
} MQTT_DISPLAY_CMD_E;

typedef struct {
    bool wifi_connected;
    bool mqtt_connected;
    char ip[16];
} MQTT_DISPLAY_STATUS_T;

OPERATE_RET        mqtt_display_start(void);
MQTT_DISPLAY_CMD_E mqtt_display_get_cmd(void);
void               mqtt_display_get_status(MQTT_DISPLAY_STATUS_T *status);

#endif /* MQTT_DISPLAY_H */
