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

#include "voice.h"

#include "app.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button.h"
#include "config_store.h"
#include "led_status.h"
#if CONFIG_HOMEHUB_MENU
#include "menu.h"
#endif
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#include "wifi_mgr.h"

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      CONFIG_HOMEHUB_VOICE_MIN_RECORD_MS
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms
#define REPLY_PAGE_CHECK_MS 500

// EVT_CHIME: a note came in; chimes between turns, and is dropped during one.
typedef enum { EVT_PRESS, EVT_RELEASE, EVT_CHIME } voice_evt_t;

static QueueHandle_t s_events;
static atomic_bool s_ready;
static atomic_int s_volume;
static atomic_bool s_muted;    // the speaker silenced, the volume kept for after
static atomic_bool s_mic_off;  // the paddle sends nothing (it keeps its setup role)
static esp_timer_handle_t s_volume_save;  // stores the volume once turning stops

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// On the esp_timer task, whose stack is in internal RAM: storing writes NVS.
static void save_volume(void *arg) {
    (void)arg;
    int volume = atomic_load(&s_volume);
    if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
    else ESP_LOGW(TAG, "volume could not be stored");
}

void voice_turn_volume(int steps) {
    if (!atomic_load(&s_ready)) return;
    atomic_store(&s_muted, false);  // turning the volume is wanting to hear it
    int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    // At an end, show it again rather than nothing, so the turn is answered.
    apply_volume(volume);
    if (s_volume_save) {
        esp_timer_stop(s_volume_save);  // fails harmlessly when it isn't running
        esp_timer_start_once(s_volume_save, VOLUME_SAVE_MS * 1000LL);
    }
}

int voice_volume(void) {
    return atomic_load(&s_volume);
}

void voice_set_muted(bool muted) {
    atomic_store(&s_muted, muted);
    ESP_LOGI(TAG, "speaker %s", muted ? "muted" : "unmuted");
}

bool voice_muted(void) {
    return atomic_load(&s_muted);
}

bool voice_speaker_on(void) {
    return atomic_load(&s_volume) > 0 && !atomic_load(&s_muted);
}

void voice_set_mic(bool on) {
    atomic_store(&s_mic_off, !on);
    ESP_LOGI(TAG, "mic %s", on ? "on" : "off");
}

bool voice_mic_on(void) {
    return !atomic_load(&s_mic_off);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

// Did a press arrive? Releases are dropped.
static bool pressed_again(TickType_t wait) {
    voice_evt_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt == EVT_PRESS) return true;
        wait = 0;
    }
    return false;
}

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed.
static size_t record(void) {
    static int16_t chunk[CAPTURE_CHUNK];
    if (voice_board_mic_start() != ESP_OK) return 0;
    size_t samples = 0;
    int64_t stop_at = 0;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) break;
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
    voice_board_mic_stop();
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

#if CONFIG_HOMEHUB_VOICE_CUES
#define CUE_MS       120
#define CUE_FADE_MS  12
#define CUE_LEVEL    12000

// A short tone on the speaker: high when recording starts, low when it stops.
// Faded in and out so it doesn't click.
static void play_cue(bool start) {
    if (!voice_speaker_on()) return;
    static int16_t tone[VOICE_PLAYER_RATE * CUE_MS / 1000];
    const int n = sizeof(tone) / sizeof(tone[0]);
    const int fade = VOICE_PLAYER_RATE * CUE_FADE_MS / 1000;
    const float hz = start ? 880.0f : 587.0f;
    for (int i = 0; i < n; i++) {
        int edge = i < n - 1 - i ? i : n - 1 - i;
        float gain = edge < fade ? (float)edge / fade : 1.0f;
        tone[i] = (int16_t)(CUE_LEVEL * gain * sinf(2.0f * (float)M_PI * hz * i / VOICE_PLAYER_RATE));
    }
    voice_player_begin();
    voice_player_write(tone, n);
    voice_player_end();
}
#endif

#if CONFIG_HOMEHUB_NOTE_COMMAND
#define CHIME_NOTE_MS  140
#define CHIME_FADE_MS  20
#define CHIME_LEVEL    10000

// Two rising notes (E6 then A6), each faded so they ring rather than click:
// unlike the cues, which say "recording", this says "something for you".
static void play_chime(void) {
    if (!voice_speaker_on()) return;
    static int16_t tone[VOICE_PLAYER_RATE * CHIME_NOTE_MS / 1000];
    static const float notes_hz[] = {1318.5f, 1760.0f};
    const int n = sizeof(tone) / sizeof(tone[0]);
    const int fade_in = VOICE_PLAYER_RATE * CHIME_FADE_MS / 1000;
    voice_player_begin();
    for (size_t k = 0; k < sizeof(notes_hz) / sizeof(notes_hz[0]); k++) {
        for (int i = 0; i < n; i++) {
            // A quick fade in, then a long linear decay to silence.
            float gain = i < fade_in ? (float)i / fade_in : (float)(n - 1 - i) / (n - fade_in);
            tone[i] = (int16_t)(CHIME_LEVEL * gain
                                * sinf(2.0f * (float)M_PI * notes_hz[k] * i / VOICE_PLAYER_RATE));
        }
        voice_player_write(tone, n);
    }
    voice_player_end();
    voice_player_wait(CHIME_NOTE_MS * 4);
}
#endif

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
    led_status_set_voice(LED_VOICE_ERROR);
    return false;
}

// Wrap `text` (Markdown cleaned off in place) to the card's width. False if
// nothing's left to show.
static bool wrap_for_card(char *text, char *lines, size_t cap) {
    muse_hatch_plain_text(text);
    return muse_hatch_wrap(text, lines, cap) > 0;
}

// Put the reply so far on the screen, on boards with a reply card. The
// message is cleaned once it's complete; until then, the copy is.
static void show_reply(void) {
    static char text[LED_STATUS_CARD_MAX];
    static char lines[LED_STATUS_CARD_MAX];
    if (!muse_hatch_turn_text(text, sizeof(text))) return;
    if (wrap_for_card(text, lines, sizeof(lines))) led_status_show_reply(lines);
}

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
    char text[96];
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    int64_t next_page = 0;
    voice_player_begin();
    for (;;) {
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
            return true;
        }
        // The page can change with no event (the reply grows past it, or its
        // Markdown is cleaned up once it's complete), so look again now and then.
        if (!done && esp_timer_get_time() >= next_page) {
            show_reply();
            next_page = esp_timer_get_time() + REPLY_PAGE_CHECK_MS * 1000LL;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
                if (!voice_player_started()) led_status_set_voice(LED_VOICE_BUFFERING);
                show_reply();
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                show_reply();  // the whole reply is in now
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
        if (n) {
            voice_player_write(pcm, n);
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    voice_player_end();
    while (!voice_player_wait(0)) {
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
#if CONFIG_HOMEHUB_VOICE_CUES
    // Record only once it's over, or the note starts with the beep.
    play_cue(true);
    voice_player_wait(CUE_MS * 3);
#endif
    muse_hatch_turn_begin();
    size_t samples = record();
    if (samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
        muse_hatch_turn_cancel();
        if (!samples) return fail("microphone unavailable");
        ESP_LOGI(TAG, "press too short");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
#if CONFIG_HOMEHUB_VOICE_CUES
    // Let it finish: the reply's playback would cut it off.
    play_cue(false);
    voice_player_wait(CUE_MS * 3);
#endif
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
#if CONFIG_HOMEHUB_MENU
    if (menu_take_paddle(pressed)) return true;  // it closed the menu instead
#endif
    if (pressed && atomic_load(&s_mic_off)) return false;  // the button's setup role
    if (pressed) {
        if (!atomic_load(&s_ready) || voice_board_muted()) return false;
        voice_hatch_refresh();
        if (!muse_hatch_ready()) return false;
        wifi_mgr_power_hold(true);  // replies come back at full speed
        app_note_activity();
    }
    voice_evt_t evt = pressed ? EVT_PRESS : EVT_RELEASE;
    xQueueSend(s_events, &evt, 0);
    return true;
}

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_ready, true);
    // It polls every 20 ms, which would keep the chip out of light sleep.
    if (voice_board_has_dial() && xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
    ESP_LOGI(TAG, "ready");

    for (;;) {
        voice_evt_t evt;
        if (xQueueReceive(s_events, &evt, portMAX_DELAY) != pdTRUE) continue;
#if CONFIG_HOMEHUB_NOTE_COMMAND
        if (evt == EVT_CHIME) {
            play_chime();
            continue;
        }
#endif
        if (evt != EVT_PRESS) continue;
        while (run_turn()) {
        }
        wifi_mgr_power_hold(false);  // the turn and its reply are over
        app_note_activity();          // idle time counts from the reply's end
    }
}

void voice_init(void) {
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    const esp_timer_create_args_t save = {.callback = save_volume, .name = "volume_save"};
    if (esp_timer_create(&save, &s_volume_save) != ESP_OK) s_volume_save = NULL;
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
    if (xTaskCreateWithCaps(voice_task, "voice", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else {
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else if (atomic_load(&s_ready)) apply_volume(volume->valueint);
            else atomic_store(&s_volume, volume->valueint);
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}

#if CONFIG_HOMEHUB_NOTE_COMMAND
#define NOTE_MAX_CHARS 1000

static cJSON *note_error(const char *why) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", "invalid_params");
    cJSON_AddStringToObject(error, "message", why);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

// Runs on the Noise session's task: the wrapping is quick, and the chime is
// left to the voice task. Its buffers are its own, apart from show_reply's,
// which runs on the voice task.
cJSON *voice_note_command(cJSON *params) {
    static char text[NOTE_MAX_CHARS + 1];
    static char lines[LED_STATUS_CARD_MAX];
    cJSON *item = cJSON_GetObjectItem(params, "text");
    if (!cJSON_IsString(item) || !item->valuestring[0]) return note_error("text is required");
    if (strlen(item->valuestring) > NOTE_MAX_CHARS) return note_error("text is over 1000 characters");
    strlcpy(text, item->valuestring, sizeof(text));
    if (!wrap_for_card(text, lines, sizeof(lines))) {
        return note_error("text is empty once its Markdown is removed");
    }
    led_status_show_note(lines);
    ESP_LOGI(TAG, "note: %s", text);

    if (atomic_load(&s_ready)) {
        voice_evt_t evt = EVT_CHIME;
        xQueueSend(s_events, &evt, 0);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    return result;
}
#endif
