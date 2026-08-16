/*
 * SPDX-FileCopyrightText: 2026 Manus AI
 * SPDX-License-Identifier: MIT
 *
 * M5Stack AtomS3 Lite + Unit Joystick v1.1 BLE keyboard.
 *
 * The joystick's X/Y deflection is reported to the paired host as standard
 * HID arrow keys. Diagonal deflection sends the two matching arrow keys
 * simultaneously. Pressing the joystick's Z button sends Enter.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"

#include "esp_hidd_prf_api.h"
#include "hid_dev.h"

#define TAG "ATOM_JOY_KB"

/* AtomS3 Lite HY2.0-4P port: Yellow = SDA (G2), White = SCL (G1). */
#define I2C_SDA_GPIO            GPIO_NUM_2
#define I2C_SCL_GPIO            GPIO_NUM_1
#define I2C_PORT_NUM            I2C_NUM_0
#define I2C_FREQUENCY_HZ        100000

/* M5Stack Unit Joystick v1.1 settings. */
#define JOYSTICK_I2C_ADDRESS    0x52
#define JOYSTICK_REPORT_SIZE    3
#define JOYSTICK_POLL_MS        25
#define JOYSTICK_STABLE_SAMPLES 2
#define AXIS_LOW_THRESHOLD      80
#define AXIS_HIGH_THRESHOLD     175

#define HIDD_DEVICE_NAME        "AtomS3 Joystick KB"
#define HIDD_APPEARANCE         0x03c0  /* HID Generic */

/* Connection ID 0 is valid, so track its validity separately. */
static volatile uint16_t s_hid_conn_id;
static volatile bool s_hid_connected;
static volatile bool s_secure_connection;
static i2c_master_dev_handle_t s_joystick;

static uint8_t s_service_uuid128[] = {
    /* LSB <--------------------------------------------------------------> MSB */
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = ESP_BLE_GAP_CONN_ITVL_MS(7.5),
    .max_interval = ESP_BLE_GAP_CONN_ITVL_MS(20),
    .appearance = HIDD_APPEARANCE,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_service_uuid128),
    .p_service_uuid = s_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = ESP_BLE_GAP_ADV_ITVL_MS(20),
    .adv_int_max = ESP_BLE_GAP_ADV_ITVL_MS(30),
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_err_t joystick_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_handle), TAG,
                        "I2C master bus initialization failed");

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = JOYSTICK_I2C_ADDRESS,
        .scl_speed_hz = I2C_FREQUENCY_HZ,
    };
    return i2c_master_bus_add_device(bus_handle, &device_config, &s_joystick);
}

static esp_err_t joystick_read(uint8_t report[JOYSTICK_REPORT_SIZE])
{
    return i2c_master_receive(s_joystick, report, JOYSTICK_REPORT_SIZE, 50);
}

static size_t joystick_to_keys(const uint8_t report[JOYSTICK_REPORT_SIZE], uint8_t keys[3])
{
    const uint8_t x = report[0];
    const uint8_t y = report[1];
    const uint8_t button = report[2];
    size_t key_count = 0;

    /* The Unit Joystick's X axis is reversed relative to its physical left/right direction. */
    /* X: low=right, high=left. Y: low=up, high=down. */
    if (x <= AXIS_LOW_THRESHOLD) {
        keys[key_count++] = HID_KEY_RIGHT_ARROW;
    } else if (x >= AXIS_HIGH_THRESHOLD) {
        keys[key_count++] = HID_KEY_LEFT_ARROW;
    }

    if (y <= AXIS_LOW_THRESHOLD) {
        keys[key_count++] = HID_KEY_UP_ARROW;
    } else if (y >= AXIS_HIGH_THRESHOLD) {
        keys[key_count++] = HID_KEY_DOWN_ARROW;
    }

    /* Unit Joystick v1.1 returns 1 when the Z button is pressed. */
    if (button == 1 && key_count < 3) {
        keys[key_count++] = HID_KEY_ENTER;
    }

    return key_count;
}

static bool keys_equal(const uint8_t *left, size_t left_count,
                       const uint8_t *right, size_t right_count)
{
    return left_count == right_count &&
           (left_count == 0 || memcmp(left, right, left_count) == 0);
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        if (param->init_finish.state == ESP_HIDD_INIT_OK) {
            ESP_ERROR_CHECK(esp_ble_gap_set_device_name(HIDD_DEVICE_NAME));
            ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&s_adv_data));
        }
        break;

    case ESP_HIDD_EVENT_BLE_CONNECT:
        s_hid_conn_id = param->connect.conn_id;
        s_hid_connected = true;
        ESP_LOGI(TAG, "BLE host connected (conn_id=%u)", s_hid_conn_id);
        break;

    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        s_hid_connected = false;
        s_secure_connection = false;
        s_hid_conn_id = 0;
        ESP_LOGI(TAG, "BLE host disconnected; restarting advertising");
        ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&s_adv_params));
        break;

    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
        /* The host may update Caps/Num/Scroll Lock LEDs; no LED is fitted. */
        break;

    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&s_adv_params));
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_ERROR_CHECK(esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true));
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        s_secure_connection = param->ble_security.auth_cmpl.success;
        ESP_LOGI(TAG, "Pairing %s", s_secure_connection ? "succeeded" : "failed");
        break;

    default:
        break;
    }
}

static void joystick_keyboard_task(void *argument)
{
    uint8_t candidate[3] = {0};
    size_t candidate_count = 0;
    uint8_t stable[3] = {0};
    size_t stable_count = 0;
    uint8_t sent[3] = {0};
    size_t sent_count = 0;
    uint8_t stable_sample_count = 0;

    while (true) {
        uint8_t report[JOYSTICK_REPORT_SIZE] = {0};
        esp_err_t err = joystick_read(report);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Joystick read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        uint8_t keys[3] = {0};
        const size_t key_count = joystick_to_keys(report, keys);

        if (keys_equal(keys, key_count, candidate, candidate_count)) {
            if (stable_sample_count < JOYSTICK_STABLE_SAMPLES) {
                stable_sample_count++;
            }
        } else {
            memcpy(candidate, keys, key_count);
            candidate_count = key_count;
            stable_sample_count = 1;
        }

        if (stable_sample_count >= JOYSTICK_STABLE_SAMPLES &&
            !keys_equal(candidate, candidate_count, stable, stable_count)) {
            memcpy(stable, candidate, candidate_count);
            stable_count = candidate_count;
        }

        if (s_hid_connected && s_secure_connection &&
            !keys_equal(stable, stable_count, sent, sent_count)) {
            esp_hidd_send_keyboard_value(s_hid_conn_id, 0, stable, stable_count);
            memcpy(sent, stable, stable_count);
            sent_count = stable_count;
        }

        if ((!s_hid_connected || !s_secure_connection) && sent_count != 0) {
            /* Prevent stale state after a host disconnects while a direction is held. */
            sent_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(JOYSTICK_POLL_MS));
    }
}

static void bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /*
     * esp_hidd_profile_init() clears the HID profile environment. It must run
     * before registering callbacks/apps, otherwise it erases the GATT interface
     * assigned by the registration event and notifications use an invalid ID.
     */
    ESP_ERROR_CHECK(esp_hidd_profile_init());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_hidd_register_callbacks(hidd_event_callback));

    const esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    const esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    const uint8_t key_size = 16;
    const uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    const uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                                   &auth_req, sizeof(auth_req)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                                   &io_cap, sizeof(io_cap)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                                   &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                                   &init_key, sizeof(init_key)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                                   &rsp_key, sizeof(rsp_key)));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(joystick_init());
    bluetooth_init();

    xTaskCreate(joystick_keyboard_task, "joystick_keyboard", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Ready. Pair with '%s'.", HIDD_DEVICE_NAME);
}
