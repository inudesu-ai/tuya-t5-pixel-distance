/**
 * @file water_effect.c
 * @brief Radar-triggered smile with temperature-controlled color
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"
#include "tkl_gpio.h"
#include "tkl_i2c.h"
#include "tkl_output.h"
#include "tkl_pinmux.h"

#include "board_com_api.h"
#include "board_pixel_api.h"
#include "tdl_audio_manage.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/
#define WATER_WIDTH           PIXEL_MATRIX_WIDTH
#define WATER_HEIGHT          PIXEL_MATRIX_HEIGHT
#define WATER_BITMAP_BYTES    (WATER_WIDTH * WATER_HEIGHT * 3)
#define WATER_FRAME_DELAY_MS  15
#define WATER_TEMP_UPDATE_MS  500
#define WATER_LOG_INTERVAL_MS 5000

#define ULTRASONIC_TRIG_PIN          TUYA_GPIO_NUM_34
#define ULTRASONIC_ECHO_PIN          TUYA_GPIO_NUM_35
#define ULTRASONIC_SAMPLE_MS         100
#define ULTRASONIC_ECHO_TIMEOUT_US   35000
#define ULTRASONIC_STALE_MS          2000
#define ULTRASONIC_ENTER_DISTANCE_CM 45.0f
#define ULTRASONIC_EXIT_DISTANCE_CM  60.0f
#define PRESENCE_CONFIRM_MS          0
#define ABSENCE_CONFIRM_MS           0
#define SMILE_MAX_MS                 5000
#define SMILE_RESET_DELAY_MS         3000
#define HAPPY_AUDIO_VOLUME           65
#define HAPPY_AUDIO_FRAME_BYTES      640

#define BME280_I2C_PORT       TUYA_I2C_NUM_0
#define BME280_ADDR_PRIMARY   0x76
#define BME280_ADDR_SECONDARY 0x77
#define BME280_REG_ID         0xD0
#define BME280_REG_CALIB_T    0x88
#define BME280_REG_CTRL_MEAS  0xF4
#define BME280_REG_TEMP_MSB   0xFA
#define BME280_CHIP_ID        0x60

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    uint8_t  address;
    uint16_t dig_t1;
    int16_t  dig_t2;
    int16_t  dig_t3;
    bool     ready;
} bme280_temp_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static PIXEL_FRAME_HANDLE_T g_frame                      = NULL;
static TDL_AUDIO_HANDLE_T   g_audio                      = NULL;
static TDL_AUDIO_INFO_T     g_audio_info                 = {0};
static bme280_temp_t        g_bme280                     = {0};
static uint8_t              g_bitmap[WATER_BITMAP_BYTES] = {0};
static float                g_temperature_c              = 24.0f;
static uint32_t             g_frame_count                = 0;

extern const unsigned char g_happy_levelup_pcm[];
extern const unsigned int  g_happy_levelup_pcm_len;
extern uint64_t            bk_aon_rtc_get_us(void);
extern void                bk_delay_us(uint32_t us);

/***********************************************************
********************function declaration********************
***********************************************************/
static float       clampf(float value, float minimum, float maximum);
static OPERATE_RET bme280_read(uint8_t address, uint8_t reg, uint8_t *data, uint8_t length);
static OPERATE_RET bme280_write(uint8_t address, uint8_t reg, uint8_t value);
static OPERATE_RET bme280_temp_init(void);
static OPERATE_RET bme280_temp_read(float *temperature_c);
static OPERATE_RET ultrasonic_init(void);
static OPERATE_RET ultrasonic_measure(float *distance_cm);
static OPERATE_RET audio_init(void);
static void        happy_sound_play(void);
static void        distance_render(float distance_cm);
static void        smile_render(void);
static void        user_main(void);

/***********************************************************
***********************function define**********************
***********************************************************/
static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int16_t read_s16_le(const uint8_t *data)
{
    return (int16_t)read_u16_le(data);
}

static OPERATE_RET bme280_read(uint8_t address, uint8_t reg, uint8_t *data, uint8_t length)
{
    OPERATE_RET rt = tkl_i2c_master_send(BME280_I2C_PORT, address, &reg, 1, FALSE);
    if (rt < 0) {
        return rt;
    }
    return tkl_i2c_master_receive(BME280_I2C_PORT, address, data, length, TRUE);
}

static OPERATE_RET bme280_write(uint8_t address, uint8_t reg, uint8_t value)
{
    uint8_t command[2] = {reg, value};
    return tkl_i2c_master_send(BME280_I2C_PORT, address, command, sizeof(command), TRUE);
}

static OPERATE_RET bme280_temp_init(void)
{
    const uint8_t       addresses[]    = {BME280_ADDR_PRIMARY, BME280_ADDR_SECONDARY};
    uint8_t             id             = 0;
    uint8_t             calibration[6] = {0};
    OPERATE_RET         rt             = OPRT_COM_ERROR;
    TUYA_IIC_BASE_CFG_T i2c_cfg        = {
               .role = TUYA_IIC_MODE_MASTER, .speed = TUYA_IIC_BUS_SPEED_100K, .addr_width = TUYA_IIC_ADDRESS_7BIT};

    tkl_io_pinmux_config(TUYA_GPIO_NUM_20, TUYA_IIC0_SCL);
    tkl_io_pinmux_config(TUYA_GPIO_NUM_21, TUYA_IIC0_SDA);
    rt = tkl_i2c_init(BME280_I2C_PORT, &i2c_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("BME280 I2C initialization failed: %d", rt);
        return rt;
    }

    for (uint32_t i = 0; i < sizeof(addresses); i++) {
        rt = bme280_read(addresses[i], BME280_REG_ID, &id, 1);
        if (rt == OPRT_OK && id == BME280_CHIP_ID) {
            g_bme280.address = addresses[i];
            break;
        }
    }

    if (g_bme280.address == 0) {
        PR_WARN("BME280 not found; using fallback color temperature");
        return OPRT_NOT_FOUND;
    }

    rt = bme280_read(g_bme280.address, BME280_REG_CALIB_T, calibration, sizeof(calibration));
    if (rt != OPRT_OK) {
        PR_ERR("BME280 calibration read failed: %d", rt);
        return rt;
    }

    g_bme280.dig_t1 = read_u16_le(&calibration[0]);
    g_bme280.dig_t2 = read_s16_le(&calibration[2]);
    g_bme280.dig_t3 = read_s16_le(&calibration[4]);

    /* Temperature oversampling x2, pressure skipped, normal mode. */
    rt = bme280_write(g_bme280.address, BME280_REG_CTRL_MEAS, 0x43);
    if (rt != OPRT_OK) {
        PR_ERR("BME280 configuration failed: %d", rt);
        return rt;
    }

    tal_system_sleep(20);
    g_bme280.ready = true;
    PR_NOTICE("BME280 ready at I2C address 0x%02x", g_bme280.address);
    return OPRT_OK;
}

static OPERATE_RET bme280_temp_read(float *temperature_c)
{
    uint8_t     raw[3] = {0};
    OPERATE_RET rt;
    int32_t     adc_t;
    int32_t     var1;
    int32_t     var2;
    int32_t     temperature_x100;

    if (temperature_c == NULL || !g_bme280.ready) {
        return OPRT_INVALID_PARM;
    }

    rt = bme280_read(g_bme280.address, BME280_REG_TEMP_MSB, raw, sizeof(raw));
    if (rt != OPRT_OK) {
        return rt;
    }

    adc_t = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | ((int32_t)raw[2] >> 4);
    if (adc_t == 0x80000) {
        return OPRT_COM_ERROR;
    }

    var1 = ((((adc_t >> 3) - ((int32_t)g_bme280.dig_t1 << 1))) * (int32_t)g_bme280.dig_t2) >> 11;
    var2 = (((((adc_t >> 4) - (int32_t)g_bme280.dig_t1) * ((adc_t >> 4) - (int32_t)g_bme280.dig_t1)) >> 12) *
            (int32_t)g_bme280.dig_t3) >>
           14;
    temperature_x100 = ((var1 + var2) * 5 + 128) >> 8;
    *temperature_c   = (float)temperature_x100 / 100.0f;
    return OPRT_OK;
}

static OPERATE_RET ultrasonic_init(void)
{
    TUYA_GPIO_BASE_CFG_T trigger_config = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    TUYA_GPIO_BASE_CFG_T echo_config = {
        .mode   = TUYA_GPIO_PULLDOWN,
        .direct = TUYA_GPIO_INPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    OPERATE_RET rt = tkl_gpio_init(ULTRASONIC_TRIG_PIN, &trigger_config);

    if (rt != OPRT_OK) {
        PR_ERR("CS100A TRIG P34 initialization failed: %d", rt);
        return rt;
    }
    rt = tkl_gpio_init(ULTRASONIC_ECHO_PIN, &echo_config);
    if (rt != OPRT_OK) {
        PR_ERR("CS100A ECHO P35 initialization failed: %d", rt);
        return rt;
    }

    tkl_gpio_write(ULTRASONIC_TRIG_PIN, TUYA_GPIO_LEVEL_LOW);
    PR_NOTICE("CS100A ready: TRIG=P34 ECHO=P35, enter<%.0fcm exit>%.0fcm", ULTRASONIC_ENTER_DISTANCE_CM,
              ULTRASONIC_EXIT_DISTANCE_CM);
    return OPRT_OK;
}

static OPERATE_RET ultrasonic_measure(float *distance_cm)
{
    TUYA_GPIO_LEVEL_E echo_level = TUYA_GPIO_LEVEL_LOW;
    uint64_t          wait_start_us;
    uint64_t          pulse_start_us;
    uint64_t          pulse_end_us;

    if (distance_cm == NULL) {
        return OPRT_INVALID_PARM;
    }

    tkl_gpio_write(ULTRASONIC_TRIG_PIN, TUYA_GPIO_LEVEL_LOW);
    bk_delay_us(3);
    tkl_gpio_write(ULTRASONIC_TRIG_PIN, TUYA_GPIO_LEVEL_HIGH);
    bk_delay_us(12);
    tkl_gpio_write(ULTRASONIC_TRIG_PIN, TUYA_GPIO_LEVEL_LOW);

    wait_start_us = bk_aon_rtc_get_us();
    do {
        if (tkl_gpio_read(ULTRASONIC_ECHO_PIN, &echo_level) != OPRT_OK) {
            return OPRT_COM_ERROR;
        }
        if (bk_aon_rtc_get_us() - wait_start_us > ULTRASONIC_ECHO_TIMEOUT_US) {
            return OPRT_TIMEOUT;
        }
    } while (echo_level == TUYA_GPIO_LEVEL_LOW);

    pulse_start_us = bk_aon_rtc_get_us();
    do {
        if (tkl_gpio_read(ULTRASONIC_ECHO_PIN, &echo_level) != OPRT_OK) {
            return OPRT_COM_ERROR;
        }
        pulse_end_us = bk_aon_rtc_get_us();
        if (pulse_end_us - pulse_start_us > ULTRASONIC_ECHO_TIMEOUT_US) {
            return OPRT_TIMEOUT;
        }
    } while (echo_level == TUYA_GPIO_LEVEL_HIGH);

    /* Round-trip time at approximately 343 m/s: distance_cm = pulse_us * 0.01715. */
    *distance_cm = (float)(pulse_end_us - pulse_start_us) * 0.01715f;
    return OPRT_OK;
}

static void audio_input_discard(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;
    (void)data;
    (void)len;
}

static OPERATE_RET audio_init(void)
{
    OPERATE_RET rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio);
    if (rt != OPRT_OK) {
        PR_ERR("Audio device '%s' not found: %d", AUDIO_CODEC_NAME, rt);
        return rt;
    }

    rt = tdl_audio_open(g_audio, audio_input_discard);
    if (rt != OPRT_OK) {
        PR_ERR("Audio device open failed: %d", rt);
        g_audio = NULL;
        return rt;
    }

    rt = tdl_audio_get_info(g_audio, &g_audio_info);
    if (rt != OPRT_OK) {
        PR_WARN("Audio frame information unavailable: %d", rt);
        memset(&g_audio_info, 0, sizeof(g_audio_info));
    }

    tdl_audio_volume_set(g_audio, HAPPY_AUDIO_VOLUME);
    PR_NOTICE("Speaker ready, frame=%u bytes, volume=%u", g_audio_info.frame_size, HAPPY_AUDIO_VOLUME);
    return OPRT_OK;
}

static void happy_sound_play(void)
{
    uint32_t frame_size = g_audio_info.frame_size;

    if (g_audio == NULL) {
        return;
    }
    if (frame_size == 0) {
        frame_size = HAPPY_AUDIO_FRAME_BYTES;
    }

    PR_NOTICE("Playing happy_levelup");
    for (uint32_t offset = 0; offset < g_happy_levelup_pcm_len; offset += frame_size) {
        uint32_t    remaining = g_happy_levelup_pcm_len - offset;
        uint32_t    length    = remaining < frame_size ? remaining : frame_size;
        OPERATE_RET rt        = tdl_audio_play(g_audio, (uint8_t *)&g_happy_levelup_pcm[offset], length);

        if (rt != OPRT_OK) {
            PR_WARN("Happy sound playback failed at %u: %d", offset, rt);
            break;
        }
    }
}

static void bitmap_set_pixel(uint32_t x, uint32_t y, uint8_t red, uint8_t green, uint8_t blue)
{
    uint32_t index;

    if (x >= WATER_WIDTH || y >= WATER_HEIGHT) {
        return;
    }

    index               = (y * WATER_WIDTH + x) * 3;
    g_bitmap[index]     = red;
    g_bitmap[index + 1] = green;
    g_bitmap[index + 2] = blue;
}

static void bitmap_draw_glyph3x5(uint32_t x, uint32_t y, const uint8_t rows[5], uint32_t scale, uint8_t red,
                                 uint8_t green, uint8_t blue)
{
    for (uint32_t row = 0; row < 5; row++) {
        for (uint32_t column = 0; column < 3; column++) {
            if ((rows[row] & (1u << (2u - column))) == 0) {
                continue;
            }
            for (uint32_t dy = 0; dy < scale; dy++) {
                for (uint32_t dx = 0; dx < scale; dx++) {
                    bitmap_set_pixel(x + column * scale + dx, y + row * scale + dy, red, green, blue);
                }
            }
        }
    }
}

static void distance_render(float distance_cm)
{
    static const uint8_t digits[10][5] = {
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
    };
    static const uint8_t glyph_c[5]        = {7, 4, 4, 4, 7};
    static const uint8_t glyph_m[5]        = {5, 7, 7, 5, 5};
    static const uint8_t glyph_dash[5]     = {0, 0, 7, 0, 0};
    float                temperature_ratio = clampf((g_temperature_c - 10.0f) / 25.0f, 0.0f, 1.0f);
    float                hue               = 220.0f * (1.0f - temperature_ratio);
    uint32_t             color_r           = 0;
    uint32_t             color_g           = 0;
    uint32_t             color_b           = 0;

    board_pixel_hsv_to_rgb(hue, 0.88f, 0.92f, &color_r, &color_g, &color_b);
    memset(g_bitmap, 0, sizeof(g_bitmap));

    if (distance_cm >= 0.0f) {
        uint32_t value = (uint32_t)(distance_cm + 0.5f);
        uint8_t  shown_digits[3];
        uint32_t count;

        if (value > 999) {
            value = 999;
        }
        if (value >= 100) {
            shown_digits[0] = (uint8_t)(value / 100);
            shown_digits[1] = (uint8_t)((value / 10) % 10);
            shown_digits[2] = (uint8_t)(value % 10);
            count           = 3;
        } else if (value >= 10) {
            shown_digits[0] = (uint8_t)(value / 10);
            shown_digits[1] = (uint8_t)(value % 10);
            count           = 2;
        } else {
            shown_digits[0] = (uint8_t)value;
            count           = 1;
        }

        uint32_t width   = count * 6 + (count - 1) * 2;
        uint32_t start_x = (WATER_WIDTH - width) / 2;
        for (uint32_t i = 0; i < count; i++) {
            bitmap_draw_glyph3x5(start_x + i * 8, 5, digits[shown_digits[i]], 2, (uint8_t)color_r, (uint8_t)color_g,
                                 (uint8_t)color_b);
        }
    } else {
        bitmap_draw_glyph3x5(9, 5, glyph_dash, 2, (uint8_t)color_r, (uint8_t)color_g, (uint8_t)color_b);
        bitmap_draw_glyph3x5(17, 5, glyph_dash, 2, (uint8_t)color_r, (uint8_t)color_g, (uint8_t)color_b);
    }

    bitmap_draw_glyph3x5(12, 20, glyph_c, 1, 170, 190, 210);
    bitmap_draw_glyph3x5(17, 20, glyph_m, 1, 170, 190, 210);
    board_pixel_frame_clear(g_frame);
    board_pixel_draw_bitmap(g_frame, 0, 0, g_bitmap, WATER_WIDTH, WATER_HEIGHT);
    board_pixel_frame_render(g_frame);
    g_frame_count++;
}

static void smile_render(void)
{
    float    temperature_ratio = clampf((g_temperature_c - 10.0f) / 25.0f, 0.0f, 1.0f);
    float    hue               = 190.0f * (1.0f - temperature_ratio) + 42.0f * temperature_ratio;
    uint32_t face_r            = 0;
    uint32_t face_g            = 0;
    uint32_t face_b            = 0;

    board_pixel_hsv_to_rgb(hue, 0.84f, 0.88f, &face_r, &face_g, &face_b);
    memset(g_bitmap, 0, sizeof(g_bitmap));

    /* A solid, symmetric 28-pixel face reads clearly on the 32x32 matrix. */
    for (uint32_t x = 0; x < WATER_WIDTH; x++) {
        for (uint32_t y = 0; y < WATER_HEIGHT; y++) {
            float dx        = (float)x - 15.5f;
            float dy        = (float)y - 15.5f;
            float distance2 = dx * dx + dy * dy;

            if (distance2 <= 190.0f) {
                float edge = distance2 > 165.0f ? 0.64f : 1.0f;
                bitmap_set_pixel(x, y, (uint8_t)((float)face_r * edge), (uint8_t)((float)face_g * edge),
                                 (uint8_t)((float)face_b * edge));
            }
        }
    }

    /* Dark oval eyes with one bright catchlight. */
    for (uint32_t y = 9; y <= 13; y++) {
        for (uint32_t x = 9; x <= 11; x++) {
            bitmap_set_pixel(x, y, 8, 12, 18);
            bitmap_set_pixel(31 - x, y, 8, 12, 18);
        }
    }
    bitmap_set_pixel(9, 9, 235, 250, 255);
    bitmap_set_pixel(22, 9, 235, 250, 255);

    /* Soft symmetric cheeks. */
    for (uint32_t x = 7; x <= 9; x++) {
        bitmap_set_pixel(x, 18, 255, 74, 105);
        bitmap_set_pixel(31 - x, 18, 255, 74, 105);
    }

    /* A two-pixel-thick upward-curved smile. */
    static const uint8_t smile_y[] = {18, 19, 20, 21, 22, 23, 23};
    for (uint32_t i = 0; i < sizeof(smile_y); i++) {
        uint32_t left_x  = 9 + i;
        uint32_t right_x = 22 - i;
        bitmap_set_pixel(left_x, smile_y[i], 12, 8, 16);
        bitmap_set_pixel(left_x, smile_y[i] + 1, 12, 8, 16);
        bitmap_set_pixel(right_x, smile_y[i], 12, 8, 16);
        bitmap_set_pixel(right_x, smile_y[i] + 1, 12, 8, 16);
    }
    for (uint32_t x = 13; x <= 18; x++) {
        bitmap_set_pixel(x, 24, 12, 8, 16);
    }

    board_pixel_frame_clear(g_frame);
    board_pixel_draw_bitmap(g_frame, 0, 0, g_bitmap, WATER_WIDTH, WATER_HEIGHT);
    board_pixel_frame_render(g_frame);
    g_frame_count++;
}

static void user_main(void)
{
    OPERATE_RET rt;
    uint32_t    last_temp_ms             = 0;
    uint32_t    last_log_ms              = 0;
    uint32_t    last_ultrasonic_ms       = 0;
    uint32_t    last_valid_distance_ms   = 0;
    uint32_t    smile_until_ms           = 0;
    uint32_t    presence_candidate_since = 0;
    float       distance_cm              = -1.0f;
    bool        presence_candidate       = false;
    bool        presence                 = false;
    bool        has_valid_distance       = false;
    bool        ultrasonic_ready         = false;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("Tuya T5AI Pixel CS100A proximity demo");

    rt = board_register_hardware();
    if (rt != OPRT_OK) {
        PR_ERR("board_register_hardware failed: %d", rt);
        return;
    }

    tal_system_sleep(200);
    bme280_temp_init();
    ultrasonic_ready = (ultrasonic_init() == OPRT_OK);
    audio_init();

    g_frame = board_pixel_frame_create();
    if (g_frame == NULL) {
        PR_ERR("Pixel frame allocation failed");
        return;
    }

    while (1) {
        uint32_t now_ms = (uint32_t)tal_system_get_millisecond();

        if (now_ms - last_temp_ms >= WATER_TEMP_UPDATE_MS) {
            float measured_temperature = 0.0f;
            if (bme280_temp_read(&measured_temperature) == OPRT_OK) {
                measured_temperature = clampf(measured_temperature, -20.0f, 60.0f);
                g_temperature_c += (measured_temperature - g_temperature_c) * 0.15f;
            }
            last_temp_ms = now_ms;
        }

        if (ultrasonic_ready && now_ms - last_ultrasonic_ms >= ULTRASONIC_SAMPLE_MS) {
            float       measured_distance_cm = 0.0f;
            OPERATE_RET measure_rt           = ultrasonic_measure(&measured_distance_cm);
            bool        detected             = presence;

            if (measure_rt == OPRT_OK && measured_distance_cm >= 2.0f && measured_distance_cm <= 500.0f) {
                if (!has_valid_distance) {
                    distance_cm = measured_distance_cm;
                } else {
                    distance_cm = distance_cm * 0.65f + measured_distance_cm * 0.35f;
                }
                has_valid_distance     = true;
                last_valid_distance_ms = now_ms;
                detected               = presence ? measured_distance_cm <= ULTRASONIC_EXIT_DISTANCE_CM
                                                  : measured_distance_cm <= ULTRASONIC_ENTER_DISTANCE_CM;
            } else if (!has_valid_distance || now_ms - last_valid_distance_ms > ULTRASONIC_STALE_MS) {
                detected = false;
            }

            if (detected != presence_candidate) {
                presence_candidate       = detected;
                presence_candidate_since = now_ms;
            }

            uint32_t confirm_ms = presence_candidate ? PRESENCE_CONFIRM_MS : ABSENCE_CONFIRM_MS;
            if (presence != presence_candidate && now_ms - presence_candidate_since >= confirm_ms) {
                presence = presence_candidate;
                PR_NOTICE("CS100A proximity: %s, distance=%.1fcm", presence ? "near" : "clear", distance_cm);

                if (presence) {
                    smile_until_ms = now_ms + SMILE_MAX_MS;
                    smile_render();
                    happy_sound_play();
                } else if ((int32_t)(smile_until_ms - now_ms) > 0) {
                    smile_until_ms = now_ms + SMILE_RESET_DELAY_MS;
                    PR_NOTICE("Smile reset in %u ms", SMILE_RESET_DELAY_MS);
                }
            }

            last_ultrasonic_ms = now_ms;
        }

        float shown_distance_cm =
            has_valid_distance && now_ms - last_valid_distance_ms <= ULTRASONIC_STALE_MS ? distance_cm : -1.0f;

        if (now_ms - last_log_ms >= WATER_LOG_INTERVAL_MS) {
            PR_NOTICE("status: temp=%.2fC distance=%.1fcm presence=%u", g_temperature_c, shown_distance_cm, presence);
            last_log_ms = now_ms;
        }

        if ((int32_t)(smile_until_ms - now_ms) > 0) {
            smile_render();
        } else {
            distance_render(shown_distance_cm);
        }
        tal_system_sleep(WATER_FRAME_DELAY_MS);
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else
static THREAD_HANDLE g_app_thread = NULL;

static void water_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(g_app_thread);
    g_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_config = {0};

    thread_config.stackDepth = 1024 * 6;
    thread_config.priority   = THREAD_PRIO_1;
    thread_config.thrdname   = "water_effect";
    tal_thread_create_and_start(&g_app_thread, NULL, NULL, water_app_thread, NULL, &thread_config);
}
#endif
