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

// Pins: Waveshare's V2 examples (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2: main/user_config.h).

#include "board_154_i2c.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.i2c";

#define I2C_PIN_SDA 47
#define I2C_PIN_SCL 48

enum { BUS_NONE, BUS_STARTING, BUS_DONE };

i2c_master_bus_handle_t board_154_i2c_bus(void) {
    static i2c_master_bus_handle_t s_bus;
    static atomic_int s_state = BUS_NONE;
    int expected = BUS_NONE;
    // The display task and the voice task can both ask first: one sets the
    // bus up, the other waits for it.
    if (atomic_compare_exchange_strong(&s_state, &expected, BUS_STARTING)) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = I2C_PIN_SDA,
            .scl_io_num = I2C_PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
            s_bus = NULL;
        }
        atomic_store(&s_state, BUS_DONE);
    }
    while (atomic_load(&s_state) != BUS_DONE) vTaskDelay(1);
    return s_bus;
}
