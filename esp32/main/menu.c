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
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app.h"
#include "led_status.h"
#include "muse_chat.h"
#include "voice.h"

static const char *TAG = "link.menu";

// Closes by itself after this long untouched.
#define MENU_TIMEOUT_MS 15000

#ifndef FIRMWARE_COMMIT
#define FIRMWARE_COMMIT "unknown"  // set by main/CMakeLists.txt
#endif

typedef enum {
    ITEM_VOLUME,   // a press starts adjusting it with the dial, another stops
    ITEM_MUTE,     // a press toggles it, as do the two below
    ITEM_MIC,
#if !CONFIG_MUSE_ENABLED
    ITEM_WIFI,
#endif
#if CONFIG_HOMEHUB_DEEP_SLEEP_IDLE_MIN > 0
    ITEM_SLEEP,    // asks for a second press, then deep-sleeps
#endif
    ITEM_RESTART,  // asks for a second press, then restarts
    ITEM_ABOUT,    // closes the menu for a card: name, credit, firmware
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
            case ITEM_MUTE:
                strlcpy(rows[i].label, "Mute", sizeof(rows[i].label));
                strlcpy(rows[i].value, voice_muted() ? "On" : "Off", sizeof(rows[i].value));
                break;
            case ITEM_MIC:
                strlcpy(rows[i].label, "Mic", sizeof(rows[i].label));
                strlcpy(rows[i].value, voice_mic_on() ? "On" : "Off", sizeof(rows[i].value));
                break;
#if !CONFIG_MUSE_ENABLED
            case ITEM_WIFI:
                strlcpy(rows[i].label, "Wi-Fi", sizeof(rows[i].label));
                strlcpy(rows[i].value, app_wifi_on() ? "On" : "Off", sizeof(rows[i].value));
                break;
#endif
#if CONFIG_HOMEHUB_DEEP_SLEEP_IDLE_MIN > 0
            case ITEM_SLEEP:
                strlcpy(rows[i].label, "Sleep now", sizeof(rows[i].label));
                break;
#endif
            case ITEM_RESTART:
                strlcpy(rows[i].label, "Restart", sizeof(rows[i].label));
                break;
            case ITEM_ABOUT:
                strlcpy(rows[i].label, "About", sizeof(rows[i].label));
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

// `text` wrapped to the card, then a blank line; nothing if it's empty.
static size_t add_paragraph(char *card, size_t at, size_t cap, const char *text) {
    char copy[64], lines[128];
    strlcpy(copy, text, sizeof(copy));
    if (!copy[0] || !muse_hatch_wrap(copy, lines, sizeof(lines))) return at;
    int n = snprintf(card + at, cap - at, "%s\n\n", lines);
    return n > 0 && (size_t)n < cap - at ? at + n : at;
}

// The About card, until the dial's press takes it away: the name, the
// credit and the name of who made it, then the firmware's version, commit
// and build date (__DATE__ is "Oct  8 2026": the month and day).
static void show_about(void) {
    static char card[LED_STATUS_CARD_MAX];
    size_t at = add_paragraph(card, 0, sizeof(card), CONFIG_HOMEHUB_ABOUT_NAME);
    char credit[160];
    snprintf(credit, sizeof(credit), "%s", CONFIG_HOMEHUB_ABOUT_CREDIT);
    if (CONFIG_HOMEHUB_ABOUT_AUTHOR[0]) {
        // The name on a line of its own, under the credit.
        size_t before = at;
        at = add_paragraph(card, at, sizeof(card), credit);
        if (at > before) at -= 1;  // one line break, not a blank line
        at = add_paragraph(card, at, sizeof(card), CONFIG_HOMEHUB_ABOUT_AUTHOR);
    } else {
        at = add_paragraph(card, at, sizeof(card), credit);
    }
    snprintf(card + at, sizeof(card) - at, "Firmware %s\n%s %.3s %d", CONFIG_HOMEHUB_FIRMWARE_VERSION,
             FIRMWARE_COMMIT, __DATE__, atoi(__DATE__ + 4));
    led_status_show_note(card);
}

// Flip a switch, and show what's now off on the status screen. Without
// s_lock: turning Wi-Fi off or on may wait for a join to finish.
static void toggle_item(int item) {
    switch ((item_t)item) {
        case ITEM_MUTE:
            voice_set_muted(!voice_muted());
            break;
        case ITEM_MIC:
            voice_set_mic(!voice_mic_on());
            break;
#if !CONFIG_MUSE_ENABLED
        case ITEM_WIFI:
            if (!app_set_wifi(!app_wifi_on())) ESP_LOGW(TAG, "wifi busy: not switched");
            break;
#endif
        default:
            return;
    }
#if CONFIG_MUSE_ENABLED
    led_status_set_switches(true, voice_mic_on());
#else
    led_status_set_switches(app_wifi_on(), voice_mic_on());
#endif
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
    bool toggle = false;
    switch ((item_t)item) {
        case ITEM_VOLUME:
            s_adjusting = !s_adjusting;
            break;
        case ITEM_ABOUT:
            s_open = false;
            break;
        case ITEM_MUTE:
        case ITEM_MIC:
#if !CONFIG_MUSE_ENABLED
        case ITEM_WIFI:
#endif
            toggle = true;
            break;
        default:
            s_confirming = confirmed ? -1 : item;
            if (confirmed) s_open = false;
            break;
    }
    xSemaphoreGive(s_lock);
    if (toggle) toggle_item(item);
    if (item == ITEM_ABOUT) {
        esp_timer_stop(s_timeout);
        show();  // the menu goes, and the card takes its place
        show_about();
        return;
    }
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
