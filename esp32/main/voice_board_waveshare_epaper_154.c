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

// voice_board.h on the Waveshare ESP32-S3-ePaper-1.54: one ES8311 codec does
// both directions (its mic in, the speaker header out) over a duplex I2S bus
// that the ESP32-S3 clocks at 16 kHz. GPIO42 (active low) powers the audio
// circuit and GPIO46 enables the speaker amplifier, which is on only while
// the player has audio to play.
//
// Pins and settings: Waveshare's V2 audio example
// (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2/08_Audio_Test: components/codec_board/board_cfg.txt,
// "S3_ePaper_1_54", and components/board_power_bsp). The codec set-up follows
// components/muse/boards/board_fnk0104b.c, another ES8311 board.

#include "voice_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "link.audio";

#define AUDIO_PIN_POWER 42  // low powers the codec and amplifier
#define AUDIO_PIN_AMP   46  // high enables the amplifier
#define AUDIO_PIN_SDA   47
#define AUDIO_PIN_SCL   48
#define AUDIO_PIN_MCLK  14
#define AUDIO_PIN_BCLK  15
#define AUDIO_PIN_WS    38
#define AUDIO_PIN_DOUT  45
#define AUDIO_PIN_DIN   16

#define AUDIO_RATE      VOICE_MIC_RATE  // the bus runs at the mic's rate
// The amplifier takes a moment to wake; sound sent sooner is lost, which
// swallowed the push-to-talk beeps whole.
#define AMP_WAKE_MS     60
#define MIC_GAIN_DB     30.0f           // Waveshare's example gain

// Volume to the DAC's level, listened to on the board's speaker: 60 %, the
// SDK's default, is comfortable at arm's length, and 100 % is the DAC's full
// level, already loud, with no digital boost to distort speech. The codec
// library takes 2.4 dB off each point, which it credits to the amplifier
// (es8311 hw_gain), so every value here is 2.4 dB above the level it sets.
static esp_codec_dev_vol_map_t s_volume_map[] = {
    {.vol = 0, .db_value = -93.1f},   // -95.5 dB: the DAC's quietest
    {.vol = 60, .db_value = -3.6f},   // -6 dB
    {.vol = 100, .db_value = 2.4f},   // 0 dB
};
// Samples per read or write: 20 ms.
#define CHUNK           (AUDIO_RATE / 50)
// The I2S receive ring: the mic always runs, so it holds the last 90 ms of
// sound, the tail of the start beep among it, until a recording reads it.
#define RX_DMA_DESCS    6
#define RX_DMA_FRAMES   240
#define RX_RING_FRAMES  (RX_DMA_DESCS * RX_DMA_FRAMES)

static esp_codec_dev_handle_t s_speaker, s_mic;
static bool s_mic_on;
static int16_t s_stereo[CHUNK * 2];

void voice_board_set_volume(int percent) {
    if (!s_speaker) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    esp_codec_dev_set_out_vol(s_speaker, percent);
}

// No mute switch on this board.
bool voice_board_muted(void) { return false; }

// No dial yet: the thumb dial is a later addition.
int voice_board_dial_steps(void) { return 0; }

void voice_board_amp(bool on) {
    static bool s_amp_on;
    if (on == s_amp_on) return;
    s_amp_on = on;
    gpio_set_level(AUDIO_PIN_AMP, on);
    if (on) vTaskDelay(pdMS_TO_TICKS(AMP_WAKE_MS));
}

esp_err_t voice_board_mic_start(void) {
    if (!s_mic) return ESP_ERR_INVALID_STATE;
    // Drop what the ring already holds, so the recording starts now. It
    // comes back at once, without waiting for new sound.
    for (int left = RX_RING_FRAMES; left > 0; left -= CHUNK) {
        int frames = left < CHUNK ? left : CHUNK;
        esp_codec_dev_read(s_mic, s_stereo, frames * 2 * sizeof(int16_t));
    }
    s_mic_on = true;
    return ESP_OK;
}

void voice_board_mic_stop(void) {
    s_mic_on = false;
}

size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (!s_mic_on) return 0;
    if (frames > CHUNK) frames = CHUNK;
    if (esp_codec_dev_read(s_mic, s_stereo, frames * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        return 0;
    }
    // The ES8311 has one ADC; take the left slot.
    for (size_t i = 0; i < frames; i++) {
        pcm[i] = s_stereo[2 * i];
        int magnitude = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (magnitude > *peak) *peak = magnitude;
    }
    return frames;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    // The player sends 48 kHz stereo, upsampled from 16 kHz: average each
    // three frames back to the bus's 16 kHz, 16 bits a sample.
    if (!s_speaker) return ESP_ERR_INVALID_STATE;
    if (count % 3) return ESP_ERR_INVALID_SIZE;
    int16_t out[32 * 2];  // small: the player's task stack is 3 KB
    while (count) {
        size_t n = count / 3;
        if (n > 32) n = 32;
        for (size_t i = 0; i < n; i++) {
            for (size_t c = 0; c < 2; c++) {
                int64_t sum = (int64_t)frames[6 * i + c] + frames[6 * i + 2 + c] + frames[6 * i + 4 + c];
                out[2 * i + c] = (int16_t)((sum / 3) >> 16);
            }
        }
        if (esp_codec_dev_write(s_speaker, out, n * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        frames += 6 * n;
        count -= 3 * n;
    }
    return ESP_OK;
}

static esp_err_t power_on(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << AUDIO_PIN_POWER | 1ULL << AUDIO_PIN_AMP,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    gpio_set_level(AUDIO_PIN_AMP, 0);
    gpio_set_level(AUDIO_PIN_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(20));  // let the codec's supply settle
    return err;
}

static esp_err_t start_i2s(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;  // silence, not the last samples, when the player runs dry
    chan.dma_desc_num = RX_DMA_DESCS;
    chan.dma_frame_num = RX_DMA_FRAMES;
    esp_err_t err = i2s_new_channel(&chan, tx, rx);
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din = AUDIO_PIN_DIN,
        },
    };
    if (err == ESP_OK) err = i2s_channel_init_std_mode(*tx, &std);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(*rx, &std);
    if (err == ESP_OK) err = i2s_channel_enable(*tx);
    if (err == ESP_OK) err = i2s_channel_enable(*rx);
    return err;
}

static esp_err_t start_codec(i2s_chan_handle_t tx, i2s_chan_handle_t rx) {
    i2c_master_bus_handle_t bus;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_PIN_SDA,
        .scl_io_num = AUDIO_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) return ESP_ERR_NO_MEM;

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,  // voice_board_amp() drives it, only while playing
        .use_mclk = true,
        .hw_gain = {.pa_gain = 6},  // Waveshare's board_cfg.txt
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    if (!codec) {
        ESP_LOGE(TAG, "ES8311 not answering on I2C");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if,
    };
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if,
    };
    s_speaker = esp_codec_dev_new(&out_cfg);
    s_mic = esp_codec_dev_new(&in_cfg);
    if (!s_speaker || !s_mic) return ESP_ERR_NO_MEM;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_speaker, &fs) != ESP_CODEC_DEV_OK
        || esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_vol_curve_t curve = {
        .vol_map = s_volume_map,
        .count = sizeof(s_volume_map) / sizeof(s_volume_map[0]),
    };
    if (esp_codec_dev_set_vol_curve(s_speaker, &curve) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "volume curve not set; the library's default applies");
    }
    return ESP_OK;
}

esp_err_t voice_board_init(void) {
    i2s_chan_handle_t tx, rx;
    esp_err_t err = power_on();
    if (err == ESP_OK) err = start_i2s(&tx, &rx);
    if (err == ESP_OK) err = start_codec(tx, rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ES8311 ready: %d Hz, mic gain %.0f dB; BOOT is push-to-talk", AUDIO_RATE,
             (double)MIC_GAIN_DB);
    return ESP_OK;
}
