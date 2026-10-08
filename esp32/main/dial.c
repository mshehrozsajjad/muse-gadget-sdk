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

// Nothing here polls, so the chip light-sleeps between uses. Like the button
// with CONFIG_HOMEHUB_BUTTON_WAKE, the push waits on a low-level interrupt
// that also wakes the chip, and polls only while it's down.
//
// The rotation is decoded in software rather than by the pulse counter: that
// holds the chip out of light sleep while enabled, and would miss the edge
// that wakes it. Each of A and B has a level interrupt armed for the level
// it isn't at, so every change of either interrupts (and wakes) once, and a
// state machine turns full quadrature cycles into clicks. It ignores
// contact bounce and any step it can't follow, so a missed edge costs at
// most that click.

#include "dial.h"

#include <stdatomic.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "link.dial";

#define DIAL_PUSH       CONFIG_HOMEHUB_DIAL_PUSH_GPIO
#define DIAL_A          CONFIG_HOMEHUB_DIAL_A_GPIO
#define DIAL_B          CONFIG_HOMEHUB_DIAL_B_GPIO
#define POLL_MS         10
#define DEBOUNCE_MS     30   // down this long to count as a press
// Which way counts as forward: +1 or -1, to match the wiring. On the Yolk
// board, -1 makes clockwise forward.
#define DIAL_DIRECTION  -1

#define NOTIFY_PUSH     (1u << 0)
#define NOTIFY_TURN     (1u << 1)

// Full-step quadrature decoder (after Ben Buxton's rotary encoder state
// machine): the state, then the input (B << 1 | A). At rest both contacts
// are open, so both pins read high (input 3). A click is reported only on
// the way back to rest, once a whole cycle has gone by one way.
enum {
    Q_START, Q_CW_FINAL, Q_CW_BEGIN, Q_CW_NEXT, Q_CCW_BEGIN, Q_CCW_FINAL, Q_CCW_NEXT,
};
#define Q_CW   0x10
#define Q_CCW  0x20
static const uint8_t s_quad_table[7][4] = {
    [Q_START]     = {Q_START,    Q_CW_BEGIN,  Q_CCW_BEGIN, Q_START},
    [Q_CW_FINAL]  = {Q_CW_NEXT,  Q_START,     Q_CW_FINAL,  Q_START | Q_CW},
    [Q_CW_BEGIN]  = {Q_CW_NEXT,  Q_CW_BEGIN,  Q_START,     Q_START},
    [Q_CW_NEXT]   = {Q_CW_NEXT,  Q_CW_BEGIN,  Q_CW_FINAL,  Q_START},
    [Q_CCW_BEGIN] = {Q_CCW_NEXT, Q_START,     Q_CCW_BEGIN, Q_START},
    [Q_CCW_FINAL] = {Q_CCW_NEXT, Q_CCW_FINAL, Q_START,     Q_START | Q_CCW},
    [Q_CCW_NEXT]  = {Q_CCW_NEXT, Q_CCW_FINAL, Q_CCW_BEGIN, Q_START},
};

static TaskHandle_t s_task;
static dial_press_cb s_on_press;
static dial_turn_cb s_on_turn;
static uint8_t s_quad = Q_START;  // only the quadrature interrupt touches it
static atomic_int s_steps;        // clicks not yet passed on

static void notify_from_isr(uint32_t bits) {
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_task, bits, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void IRAM_ATTR on_push_low(void *arg) {
    (void)arg;
    gpio_intr_disable(DIAL_PUSH);
    notify_from_isr(NOTIFY_PUSH);
}

// Interrupt (and wake) when `pin` leaves `level`.
static void arm_for_change(int pin, int level) {
    gpio_wakeup_enable(pin, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
}

// A or B changed. The GPIO interrupt service isn't in IRAM, so calling the
// driver from here is fine.
static void on_quadrature(void *arg) {
    (void)arg;
    int a = gpio_get_level(DIAL_A), b = gpio_get_level(DIAL_B);
    arm_for_change(DIAL_A, a);
    arm_for_change(DIAL_B, b);
    s_quad = s_quad_table[s_quad & 0x0f][b << 1 | a];
    int click = s_quad & Q_CW ? 1 : s_quad & Q_CCW ? -1 : 0;
    if (click) {
        atomic_fetch_add(&s_steps, click * DIAL_DIRECTION);
        notify_from_isr(NOTIFY_TURN);
    }
}

static bool push_down(void) {
    return gpio_get_level(DIAL_PUSH) == 0;
}

// The push is down: wait it out, and pass it on if it was a press rather
// than a bounce.
static void handle_push(void) {
    int down_ms = 0;
    while (push_down() && down_ms < DEBOUNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        down_ms += POLL_MS;
    }
    if (down_ms >= DEBOUNCE_MS) {
        while (push_down()) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));  // the release bounces too
        ESP_LOGI(TAG, "press");
        if (s_on_press) s_on_press();
    }
    gpio_intr_enable(DIAL_PUSH);  // up again: wait for the next
}

static void dial_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (bits & NOTIFY_TURN) {
            int steps = atomic_exchange(&s_steps, 0);
            if (steps) {
                ESP_LOGI(TAG, "turn %+d", steps);
                if (s_on_turn) s_on_turn(steps);
            }
        }
        if (bits & NOTIFY_PUSH) handle_push();
    }
}

static esp_err_t push_init(void) {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DIAL_PUSH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_isr_handler_add(DIAL_PUSH, on_push_low, NULL);
    // Enabled from here: a press disables it until handle_push has seen it out.
    if (err == ESP_OK) err = gpio_wakeup_enable(DIAL_PUSH, GPIO_INTR_LOW_LEVEL);
    return err;
}

static esp_err_t quadrature_init(void) {
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DIAL_A | 1ULL << DIAL_B,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_isr_handler_add(DIAL_A, on_quadrature, NULL);
    if (err == ESP_OK) err = gpio_isr_handler_add(DIAL_B, on_quadrature, NULL);
    if (err == ESP_OK) {
        arm_for_change(DIAL_A, gpio_get_level(DIAL_A));
        arm_for_change(DIAL_B, gpio_get_level(DIAL_B));
        gpio_intr_enable(DIAL_A);
        gpio_intr_enable(DIAL_B);
    }
    return err;
}

bool dial_init(dial_press_cb on_press, dial_turn_cb on_turn) {
    s_on_press = on_press;
    s_on_turn = on_turn;
    // The task first: the interrupts notify it.
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the dial");
        return false;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;  // the button installed it
    if (err == ESP_OK) err = push_init();
    if (err == ESP_OK) err = quadrature_init();
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dial on GPIO %d/%d/%d: %s", DIAL_A, DIAL_B, DIAL_PUSH,
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "dial ready: A GPIO %d, B GPIO %d, push GPIO %d", DIAL_A, DIAL_B, DIAL_PUSH);
    return true;
}
