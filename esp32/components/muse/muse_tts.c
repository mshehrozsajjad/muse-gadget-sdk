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

// API: elevenlabs.io/docs/api-reference/text-to-speech/stream. mp3_22050_32 is
// one of its low-latency formats; the model is CONFIG_MUSE_TTS_ELEVENLABS_MODEL.

#include "muse_tts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "muse_tts";

#define TTS_URL_FMT \
    "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=mp3_22050_32"
// About 8 s of 32 kbps MP3 between the network and the session task.
#define MP3_BUFFER_BYTES  (32 * 1024)
#define READ_CHUNK        2048
#define HTTP_TIMEOUT_MS   15000
#define ERROR_BODY_MAX    200

static StreamBufferHandle_t s_mp3;
static SemaphoreHandle_t s_lock;    // guards s_text
static TaskHandle_t s_task;
static char *s_text;                // the next request's text, owned here
static atomic_int s_state = MUSE_TTS_IDLE;
static atomic_uint s_gen;           // bumped by every start and cancel

bool muse_tts_configured(void) {
    return CONFIG_MUSE_TTS_ELEVENLABS_API_KEY[0] && CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID[0];
}

// The request's JSON body, malloc'd; NULL without memory.
static char *request_body(const char *text) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "model_id", CONFIG_MUSE_TTS_ELEVENLABS_MODEL);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

// Logs what the API said about a refused request.
static void log_refusal(esp_http_client_handle_t c, int status) {
    char why[ERROR_BODY_MAX + 1];
    int n = esp_http_client_read(c, why, ERROR_BODY_MAX);
    why[n > 0 ? n : 0] = '\0';
    ESP_LOGW(TAG, "ElevenLabs refused the request (HTTP %d): %s", status, why);
}

// Fetches the speech for `text` into s_mp3. True if all of it arrived (or the
// fetch was abandoned for a newer one, which needs no failure).
static bool fetch(const char *text, unsigned gen) {
    char url[160];
    snprintf(url, sizeof(url), TTS_URL_FMT, CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID);
    char *body = request_body(text);
    if (!body) return false;
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        return false;
    }
    esp_http_client_set_header(c, "xi-api-key", CONFIG_MUSE_TTS_ELEVENLABS_API_KEY);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "audio/mpeg");

    bool ok = false;
    int len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK && esp_http_client_write(c, body, len) == len) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            log_refusal(c, status);
        } else {
            atomic_store(&s_state, MUSE_TTS_FETCHING);
            static uint8_t chunk[READ_CHUNK];
            size_t total = 0;
            for (;;) {
                if (atomic_load(&s_gen) != gen) {
                    ok = true;  // abandoned: nobody wants the rest
                    break;
                }
                int n = esp_http_client_read(c, (char *)chunk, sizeof(chunk));
                if (n < 0) {
                    ESP_LOGW(TAG, "speech download failed after %u bytes", (unsigned)total);
                    break;
                }
                if (n == 0) {
                    ok = esp_http_client_is_complete_data_received(c);
                    if (!ok) ESP_LOGW(TAG, "speech cut off after %u bytes", (unsigned)total);
                    break;
                }
                // The session task drains it; wait for room, unless abandoned.
                for (int sent = 0; sent < n && atomic_load(&s_gen) == gen;) {
                    sent += (int)xStreamBufferSend(s_mp3, chunk + sent, n - sent, pdMS_TO_TICKS(100));
                }
                total += n;
            }
            if (ok) ESP_LOGI(TAG, "speech: %u bytes of MP3", (unsigned)total);
        }
    } else {
        ESP_LOGW(TAG, "can't reach ElevenLabs: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    return ok;
}

static void tts_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        char *text = s_text;
        s_text = NULL;
        unsigned gen = atomic_load(&s_gen);
        xSemaphoreGive(s_lock);
        if (!text) continue;
        // Only this task writes and the session task never waits to read, so
        // nothing is blocked on the buffer and the reset succeeds.
        xStreamBufferReset(s_mp3);
        bool ok = fetch(text, gen);
        free(text);
        if (atomic_load(&s_gen) == gen) {
            atomic_store(&s_state, ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED);
        }
    }
}

bool muse_tts_start(const char *text) {
    if (!muse_tts_configured() || !text || !text[0]) return false;
    if (!s_task) {
        s_lock = xSemaphoreCreateMutex();
        s_mp3 = xStreamBufferCreateWithCaps(MP3_BUFFER_BYTES, 1, MALLOC_CAP_SPIRAM);
        // Stack in PSRAM, like the session's: TLS runs here, and the task
        // never writes flash.
        if (!s_lock || !s_mp3
            || xTaskCreatePinnedToCoreWithCaps(tts_task, "muse_tts", 8 * 1024, NULL, 4, &s_task, 1,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "no memory for speech");
            s_task = NULL;
            return false;
        }
    }
    size_t len = strlen(text) + 1;
    char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) return false;
    memcpy(copy, text, len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_text);
    s_text = copy;
    atomic_fetch_add(&s_gen, 1);
    atomic_store(&s_state, MUSE_TTS_STARTING);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

muse_tts_state_t muse_tts_state(void) {
    return (muse_tts_state_t)atomic_load(&s_state);
}

size_t muse_tts_read(uint8_t *buf, size_t cap) {
    muse_tts_state_t state = muse_tts_state();
    if (!s_mp3 || (state != MUSE_TTS_FETCHING && state != MUSE_TTS_DONE && state != MUSE_TTS_FAILED)) {
        return 0;
    }
    return xStreamBufferReceive(s_mp3, buf, cap, 0);
}

void muse_tts_cancel(void) {
    if (!s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_text);
    s_text = NULL;
    atomic_fetch_add(&s_gen, 1);
    atomic_store(&s_state, MUSE_TTS_IDLE);
    xSemaphoreGive(s_lock);
}
