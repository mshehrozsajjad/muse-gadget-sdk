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

// Like the button with CONFIG_HOMEHUB_BUTTON_WAKE: the task waits on a
// low-level interrupt, which also wakes the chip from light sleep, rather
// than polling, and polls only while the switch is down.

#include "dial.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "link.dial";

#define DIAL_PUSH       CONFIG_HOMEHUB_DIAL_PUSH_GPIO
#define POLL_MS         10
#define DEBOUNCE_MS     30   // down this long to count as a press

static TaskHandle_t s_task;
static dial_cb s_on_press;

static void IRAM_ATTR on_push_low(void *arg) {
    (void)arg;
    gpio_intr_disable(DIAL_PUSH);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static bool push_down(void) {
    return gpio_get_level(DIAL_PUSH) == 0;
}

static void dial_task(void *arg) {
    (void)arg;
    for (;;) {
        // Up: wait for the pin to go low.
        gpio_intr_enable(DIAL_PUSH);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // A press holds low past the debounce; a contact bounce doesn't.
        int down_ms = 0;
        while (push_down() && down_ms < DEBOUNCE_MS) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            down_ms += POLL_MS;
        }
        if (down_ms < DEBOUNCE_MS) continue;

        while (push_down()) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));  // the release bounces too
        ESP_LOGI(TAG, "press");
        if (s_on_press) s_on_press();
    }
}

bool dial_init(dial_cb on_press) {
    s_on_press = on_press;
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DIAL_PUSH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;  // the button installed it
    }
    if (err == ESP_OK) err = gpio_isr_handler_add(DIAL_PUSH, on_push_low, NULL);
    if (err == ESP_OK) err = gpio_wakeup_enable(DIAL_PUSH, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
    if (err == ESP_OK) gpio_intr_disable(DIAL_PUSH);  // the task enables it when waiting
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dial push on GPIO %d: %s", DIAL_PUSH, esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the dial");
        return false;
    }
    ESP_LOGI(TAG, "dial push ready on GPIO %d", DIAL_PUSH);
    return true;
}
