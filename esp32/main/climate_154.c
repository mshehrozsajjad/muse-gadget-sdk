/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Commands, conversion and the warmth correction: Waveshare's V2 SHTC3 example
// (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2/03_I2C_SHTC3/components/i2c_equipment).

#include "climate_154.h"

#include "board_154_i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.climate";

#define SHTC3_ADDR          0x70
#define SHTC3_WAKEUP        0x3517
#define SHTC3_SLEEP         0xB098
#define SHTC3_MEASURE       0x7866  // temperature first, no clock stretching
#define SHTC3_WAKE_US       240
#define SHTC3_MEASURE_MS    20      // a normal-mode measurement takes up to 12.1 ms
#define SHTC3_I2C_HZ        100000
#define SHTC3_TIMEOUT_MS    50
// The chip and the radio warm the board, and the sensor with it; Waveshare
// takes this much off.
#define BOARD_WARMTH_C      4.0f

static i2c_master_dev_handle_t s_dev;

static esp_err_t send_command(uint16_t cmd) {
    const uint8_t bytes[2] = {cmd >> 8, cmd & 0xFF};
    return i2c_master_transmit(s_dev, bytes, sizeof(bytes), SHTC3_TIMEOUT_MS);
}

// The SHTC3's CRC-8: polynomial 0x31, starting at 0xFF.
static uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = crc & 0x80 ? (uint8_t)(crc << 1 ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

bool climate_154_init(void) {
    i2c_master_bus_handle_t bus = board_154_i2c_bus();
    if (!bus) return false;
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = SHTC3_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 not added: %s", esp_err_to_name(err));
        s_dev = NULL;
        return false;
    }
    return true;
}

bool climate_154_read(float *celsius, float *humidity) {
    if (!s_dev) return false;
    uint8_t raw[6];  // temperature, its CRC, humidity, its CRC
    esp_err_t err = send_command(SHTC3_WAKEUP);
    if (err == ESP_OK) {
        esp_rom_delay_us(SHTC3_WAKE_US);
        err = send_command(SHTC3_MEASURE);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHTC3_MEASURE_MS));
        err = i2c_master_receive(s_dev, raw, sizeof(raw), SHTC3_TIMEOUT_MS);
    }
    send_command(SHTC3_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (crc8(raw, 2) != raw[2] || crc8(raw + 3, 2) != raw[5]) {
        ESP_LOGW(TAG, "SHTC3 checksum mismatch");
        return false;
    }
    *celsius = 175.0f * (float)(raw[0] << 8 | raw[1]) / 65536.0f - 45.0f - BOARD_WARMTH_C;
    *humidity = 100.0f * (float)(raw[3] << 8 | raw[4]) / 65536.0f;
    return true;
}
