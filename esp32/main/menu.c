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

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app.h"
#include "led_status.h"
#include "voice.h"

static const char *TAG = "link.menu";

// Closes by itself after this long untouched.
#define MENU_TIMEOUT_MS 15000

typedef enum {
    ITEM_VOLUME,   // a press starts adjusting it with the dial, another stops
#if CONFIG_HOMEHUB_DEEP_SLEEP_IDLE_MIN > 0
    ITEM_SLEEP,    // asks for a second press, then deep-sleeps
#endif
    ITEM_RESTART,  // asks for a second press, then restarts
    ITEM_COUNT,
} item_t;

// Guards everything below. Never held while calling out to the display or to
// voice_turn_volume, which take locks of their own, nor while sleeping or
// restarting.
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timeout;
static bool s_open;
static int s_selected;
static bool s_adjusting;   // the dial turns the selected item's value
static int s_confirming = -1;  // the item waiting for its second press, -1 for none
static bool s_paddle_held; // the paddle press that closed the menu is still down

// The rows as they stand. Caller holds s_lock.
static int menu_rows(led_menu_row_t rows[ITEM_COUNT]) {
    memset(rows, 0, ITEM_COUNT * sizeof(rows[0]));  // the display compares them whole
    for (int i = 0; i < ITEM_COUNT; i++) {
        bool confirm = s_confirming == i;
        switch ((item_t)i) {
            case ITEM_VOLUME: {
                int volume = voice_volume();
                strlcpy(rows[i].label, "Volume", sizeof(rows[i].label));
                if (volume) snprintf(rows[i].value, sizeof(rows[i].value), "%d%%", volume);
                else strlcpy(rows[i].value, "Off", sizeof(rows[i].value));
                break;
            }
#if CONFIG_HOMEHUB_DEEP_SLEEP_IDLE_MIN > 0
            case ITEM_SLEEP:
                strlcpy(rows[i].label, "Sleep now", sizeof(rows[i].label));
                break;
#endif
            case ITEM_RESTART:
                strlcpy(rows[i].label, "Restart", sizeof(rows[i].label));
                break;
            case ITEM_COUNT:
                break;
        }
        if (confirm) strlcpy(rows[i].value, "Sure?", sizeof(rows[i].value));
    }
    return ITEM_COUNT;
}

// Redraw the menu as it stands, or take it away once closed.
static void show(void) {
    led_menu_row_t rows[ITEM_COUNT];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool open = s_open;
    int count = open ? menu_rows(rows) : 0;
    int selected = s_selected;
    bool adjusting = s_adjusting;
    xSemaphoreGive(s_lock);
    led_status_show_menu(open ? rows : NULL, count, selected, adjusting);
}

// Another 15 s before it closes by itself.
static void keep_open(void) {
    esp_timer_stop(s_timeout);  // fails harmlessly when it isn't running
    esp_timer_start_once(s_timeout, MENU_TIMEOUT_MS * 1000LL);
}

static void on_timeout(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "closed: untouched");
    menu_close();
}

bool menu_init(void) {
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t timeout = {.callback = on_timeout, .name = "menu_timeout"};
    if (!s_lock || esp_timer_create(&timeout, &s_timeout) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the menu");
        return false;
    }
    return true;
}

bool menu_is_open(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool open = s_open;
    xSemaphoreGive(s_lock);
    return open;
}

void menu_open(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_open = true;
    s_selected = 0;
    s_adjusting = false;
    s_confirming = -1;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "open");
    keep_open();
    show();
}

void menu_close(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_open = s_open;
    s_open = false;
    xSemaphoreGive(s_lock);
    if (!was_open) return;
    esp_timer_stop(s_timeout);
    show();
}

void menu_turn(int steps) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) {
        xSemaphoreGive(s_lock);
        return;
    }
    bool volume = s_adjusting && s_selected == ITEM_VOLUME;
    if (!volume) {
        int selected = s_selected + steps;
        s_selected = selected < 0 ? 0 : selected >= ITEM_COUNT ? ITEM_COUNT - 1 : selected;
        s_confirming = -1;  // moving away cancels a pending second press
    }
    xSemaphoreGive(s_lock);
    if (volume) voice_turn_volume(steps);
    keep_open();
    show();
}

void menu_press(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_open) {
        xSemaphoreGive(s_lock);
        return;
    }
    int item = s_selected;
    bool confirmed = s_confirming == item;
    switch ((item_t)item) {
        case ITEM_VOLUME:
            s_adjusting = !s_adjusting;
            break;
        default:
            s_confirming = confirmed ? -1 : item;
            if (confirmed) s_open = false;
            break;
    }
    xSemaphoreGive(s_lock);
    if (confirmed) {
        esp_timer_stop(s_timeout);
        show();  // the menu goes before the screen changes
#if CONFIG_HOMEHUB_DEEP_SLEEP_IDLE_MIN > 0
        if (item == ITEM_SLEEP) {
            app_sleep_now();  // returns only if it can't
            ESP_LOGW(TAG, "can't sleep now");
            return;
        }
#endif
        if (item == ITEM_RESTART) {
            ESP_LOGI(TAG, "restart");
            esp_restart();
        }
        return;
    }
    keep_open();
    show();
}

bool menu_take_paddle(bool pressed) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool take = pressed ? s_open : s_paddle_held;
    if (pressed && take) s_paddle_held = true;
    if (!pressed) s_paddle_held = false;
    xSemaphoreGive(s_lock);
    if (pressed && take) {
        ESP_LOGI(TAG, "closed: paddle");
        menu_close();
    }
    return take;
}
