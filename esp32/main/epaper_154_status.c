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

// led_status.h on the Waveshare ESP32-S3-ePaper-1.54 (V2): a 1.54 inch
// 200x200 black and white e-paper on SPI with an SSD1681-type controller.
// Like epaper_status.c for the reTerminal, it replaces led_status.c and shows
// a still status screen (the agent's name, the character and the status
// text), redrawn only when the text changes.
//
// The board also gates its own power: GPIO17 holds the battery switch on once
// PWR is released, and GPIO6 (active low) powers the panel. Both are set here,
// first thing, since this is the earliest board-specific code that runs.
//
// Everything is drawn into an 8-bit gray PSRAM canvas, then dithered to black
// and white just before the refresh.
//
// Pins, power lines, controller commands and waveforms: Waveshare's V2
// ESP-IDF examples (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2: main/user_config.h, components/epaper_driver_bsp and
// components/board_power_bsp).

#include "led_status.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper_pixels.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "happy_anim.h"
#include "pixel_font.h"
#include "sdkconfig.h"
#include "stack_monitor.h"

static const char *TAG = "link.led";

// ---- Board ------------------------------------------------------------------

#define BOARD_PIN_VBAT_HOLD 17  // high keeps the battery switched on
#define BOARD_PIN_EPD_POWER 6   // low powers the panel

#define EPD_HOST        SPI2_HOST
#define EPD_PIN_SCLK    12
#define EPD_PIN_MOSI    13
#define EPD_PIN_CS      11
#define EPD_PIN_DC      10
#define EPD_PIN_RST     9
// High while the controller is busy (the reTerminal's is the other way).
#define EPD_PIN_BUSY    8
#define EPD_SPI_HZ      (10 * 1000 * 1000)
#define EPD_W           200
#define EPD_H           200
#define EPD_ROW_BYTES   (EPD_W / 8)
#define EPD_FRAME_BYTES (EPD_ROW_BYTES * EPD_H)
#define CANVAS_BYTES    ((size_t)EPD_W * EPD_H)
// A full refresh takes about 2 s and a fast one under 0.5 s; this is for a
// stuck or missing panel.
#define EPD_BUSY_TIMEOUT_MS 10000
// Frame data goes out through a DMA buffer this big.
#define EPD_CHUNK_BYTES 4000

// A status change waits this long for the next, so that the burst while
// connecting costs one refresh.
#define STATUS_SETTLE_MS 1500
// Fast refreshes leave a faint ghost of the old picture; every so often the
// status gets a full refresh, which flashes but clears it.
#define FULL_REFRESH_EVERY 10

// Status screen: the title on top, the character in the middle, up to two
// lines of status text below.
#define TITLE_Y          6
#define TITLE_MAX_SCALE  3
#define ANIM_SCALE       2
#define ANIM_W           (HAPPY_ANIM_WIDTH * ANIM_SCALE)
#define ANIM_X           ((EPD_W - ANIM_W) / 2)
#define ANIM_Y           36
#define STATUS_Y         160
#define STATUS_SCALE     2
#define STATUS_LINE_GAP  4

// Switch the battery hold and the panel on, and keep both through light sleep.
static void board_power_on(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << BOARD_PIN_VBAT_HOLD | 1ULL << BOARD_PIN_EPD_POWER,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_PIN_VBAT_HOLD, 1);
    gpio_set_level(BOARD_PIN_EPD_POWER, 0);
    gpio_hold_en(BOARD_PIN_VBAT_HOLD);
    gpio_hold_en(BOARD_PIN_EPD_POWER);
}

// ---- Panel ------------------------------------------------------------------

// Waveshare's full and partial waveforms (WF_Full_1IN54, WF_PARTIAL_1IN54_0):
// 153 bytes of LUT, then the gate level (0x3F), gate voltage (0x03), source
// voltages (0x04, three bytes) and VCOM (0x2C).
#define EPD_LUT_BYTES 153
static const uint8_t s_wf_full[EPD_LUT_BYTES + 6] = {
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x48, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x48, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8, 0x1, 0x0, 0x8, 0x1, 0x0, 0x2,
    0xA, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
    0x22, 0x17, 0x41, 0x0, 0x32, 0x20,
};
static const uint8_t s_wf_partial[EPD_LUT_BYTES + 6] = {
    0x0,  0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  0x0,  0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0xF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x1, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28,
};

static spi_device_handle_t s_spi;
static uint8_t *s_chunk;     // DMA buffer for SPI writes
static uint8_t *s_canvas;    // what is being drawn, 8-bit gray
static uint8_t *s_frame;     // s_canvas dithered, about to be shown
static uint8_t *s_shown;     // what the panel shows, for fast refreshes
static int16_t *s_err;       // dithering error rows
static bool s_ready;

// Guards the panel, s_frame, s_shown and s_fast_refreshes. Taken before
// s_lock, and held through a refresh, so that drawing waits only for the
// dithering, not the time the panel takes.
static SemaphoreHandle_t s_panel_lock;
static int s_fast_refreshes = FULL_REFRESH_EVERY;  // the first is full

// Guards s_canvas and the drawn-state below.
static SemaphoreHandle_t s_lock;
static bool s_image_mode;    // an image replaces the status screen
static bool s_image_dirty;   // drawn since the last refresh
static bool s_status_drawn;  // the status screen shows s_drawn_*
static const char *s_drawn_label;
static char s_drawn_title[48];

// Requested by led_status_set_state() and _set_title(); guarded by s_mutex.
static SemaphoreHandle_t s_mutex;
static led_state_t s_state = LED_STATE_BOOT;
static char s_title[48];
static TaskHandle_t s_task;

// Send `len` bytes, as data or as a command.
static esp_err_t epd_write(bool data, const uint8_t *buf, size_t len) {
    gpio_set_level(EPD_PIN_DC, data);
    while (len) {
        size_t n = len < EPD_CHUNK_BYTES ? len : EPD_CHUNK_BYTES;
        memcpy(s_chunk, buf, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = s_chunk};
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) return err;
        buf += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t epd_cmd(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t err = epd_write(false, &cmd, 1);
    if (err == ESP_OK && len) err = epd_write(true, data, len);
    return err;
}

static esp_err_t epd_wait_idle(void) {
    int64_t give_up = esp_timer_get_time() + EPD_BUSY_TIMEOUT_MS * 1000LL;
    while (gpio_get_level(EPD_PIN_BUSY) == 1) {
        if (esp_timer_get_time() > give_up) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

// Load a waveform: the LUT, then its voltages.
static esp_err_t epd_load_waveform(const uint8_t *wf) {
    esp_err_t err = epd_cmd(0x32, wf, EPD_LUT_BYTES);
    if (err == ESP_OK) err = epd_wait_idle();
    if (err == ESP_OK) err = epd_cmd(0x3F, &wf[153], 1);
    if (err == ESP_OK) err = epd_cmd(0x03, &wf[154], 1);
    if (err == ESP_OK) err = epd_cmd(0x04, &wf[155], 3);
    if (err == ESP_OK) err = epd_cmd(0x2C, &wf[158], 1);
    return err;
}

// Point the RAM address counters at the first byte of the frame.
static esp_err_t epd_set_cursor(void) {
    static const uint8_t x = 0, y[2] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8};
    esp_err_t err = epd_cmd(0x4E, &x, 1);
    if (err == ESP_OK) err = epd_cmd(0x4F, y, sizeof(y));
    return err;
}

// Wake the controller from deep sleep and set it up for a full or a fast
// refresh: Waveshare's EPD_Init and EPD_Init_Partial, with the address
// window set for both, since a reset may lose it.
static esp_err_t epd_wake(bool full) {
    // Deep sleep ends only with a reset.
    gpio_set_level(EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err = epd_wait_idle();
    if (err == ESP_OK && full) {
        err = epd_cmd(0x12, NULL, 0);  // software reset
        if (err == ESP_OK) err = epd_wait_idle();
    }
    static const uint8_t driver_output[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0x01};
    static const uint8_t entry_mode = 0x01;  // x up, y down
    static const uint8_t x_window[] = {0, (EPD_W - 1) / 8};
    static const uint8_t y_window[] = {(EPD_H - 1) & 0xFF, (EPD_H - 1) >> 8, 0, 0};
    if (err == ESP_OK) err = epd_cmd(0x01, driver_output, sizeof(driver_output));
    if (err == ESP_OK) err = epd_cmd(0x11, &entry_mode, 1);
    if (err == ESP_OK) err = epd_cmd(0x44, x_window, sizeof(x_window));
    if (err == ESP_OK) err = epd_cmd(0x45, y_window, sizeof(y_window));
    if (full) {
        static const uint8_t border = 0x01, sensor = 0x80, load = 0xB1;
        if (err == ESP_OK) err = epd_cmd(0x3C, &border, 1);
        if (err == ESP_OK) err = epd_cmd(0x18, &sensor, 1);  // internal temperature sensor
        // Load the temperature and the OTP waveform settings.
        if (err == ESP_OK) err = epd_cmd(0x22, &load, 1);
        if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
        if (err == ESP_OK) err = epd_wait_idle();
        if (err == ESP_OK) err = epd_load_waveform(s_wf_full);
    } else {
        static const uint8_t otp_off[10] = {0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0};
        static const uint8_t border = 0x80, analog_on = 0xC0;
        if (err == ESP_OK) err = epd_load_waveform(s_wf_partial);
        if (err == ESP_OK) err = epd_cmd(0x37, otp_off, sizeof(otp_off));
        if (err == ESP_OK) err = epd_cmd(0x3C, &border, 1);
        if (err == ESP_OK) err = epd_cmd(0x22, &analog_on, 1);
        if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);
        if (err == ESP_OK) err = epd_wait_idle();
    }
    return err;
}

#if CONFIG_HOMEHUB_EPAPER_154_ROTATE_180
static uint8_t reverse_bits(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++, b >>= 1) r = (uint8_t)(r << 1 | (b & 1));
    return r;
}

// Turn a packed frame upside down: the bytes in reverse order, and the bits in
// each byte too.
static void rotate_frame_180(uint8_t *bits) {
    for (size_t i = 0; i < EPD_FRAME_BYTES / 2; i++) {
        size_t j = EPD_FRAME_BYTES - 1 - i;
        uint8_t first = bits[i];
        bits[i] = reverse_bits(bits[j]);
        bits[j] = reverse_bits(first);
    }
}
#endif

// Wake the controller, send the frame and refresh, then back to deep sleep;
// the picture stays. A full refresh takes about 2 s and flashes black and
// white; a fast one is quicker, may leave a ghost, and needs the old frame,
// which deep sleep and the reset lose. Caller holds s_panel_lock.
static esp_err_t epd_update(bool full) {
    esp_err_t err = epd_wake(full);
    if (err == ESP_OK) err = epd_set_cursor();
    if (err == ESP_OK) err = epd_cmd(0x26, full ? s_frame : s_shown, EPD_FRAME_BYTES);
    if (err == ESP_OK) err = epd_set_cursor();
    if (err == ESP_OK) err = epd_cmd(0x24, s_frame, EPD_FRAME_BYTES);
    const uint8_t mode = full ? 0xC7 : 0xCF;
    int64_t start = esp_timer_get_time();
    if (err == ESP_OK) err = epd_cmd(0x22, &mode, 1);
    if (err == ESP_OK) err = epd_cmd(0x20, NULL, 0);  // refresh
    if (err == ESP_OK) err = epd_wait_idle();
    int ms = (int)((esp_timer_get_time() - start) / 1000);
    static const uint8_t sleep_mode = 0x01;
    if (err == ESP_OK) err = epd_cmd(0x10, &sleep_mode, 1);  // deep sleep, RAM kept
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-paper refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    memcpy(s_shown, s_frame, EPD_FRAME_BYTES);
    s_fast_refreshes = full ? 0 : s_fast_refreshes + 1;
    ESP_LOGI(TAG, "e-paper %s refresh in %d ms", full ? "full" : "fast", ms);
    return ESP_OK;
}

// Dither s_canvas into s_frame. Caller holds s_panel_lock and s_lock.
static void epd_dither(void) {
    dither_frame(s_canvas, s_frame, EPD_W, EPD_H, s_err);
#if CONFIG_HOMEHUB_EPAPER_154_ROTATE_180
    rotate_frame_180(s_frame);
#endif
}

// ---- Status screen ----------------------------------------------------------

static const char *status_label(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:                     return "Starting";
        case LED_STATE_SETUP_IDLE:               return "Press the button to set up";
        case LED_STATE_BLE_ADVERTISING:          return "Ready to pair";
        case LED_STATE_BLE_CONNECTED:            return "Pairing";
        case LED_STATE_PAIRING_CONFIRM_REQUIRED: return "Press the button to confirm";
        case LED_STATE_WIFI_CONNECTING:
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:                    return "Connecting";
        case LED_STATE_WS_CONNECTED:             return "Connected";
        case LED_STATE_WS_DISCONNECTED:          return "Reconnecting";
        case LED_STATE_UNPAIRED:                 return "Not paired";
        case LED_STATE_ERROR:                    return "Error";
    }
    return "";
}

// Draw the first `n` bytes of `text` in black at pixel size `scale`, centred
// on the row at `y`. Bytes outside printable ASCII show as '?'.
static void draw_run(const char *text, int n, int y, int scale) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    int x0 = (EPD_W - (n * adv * scale - scale)) / 2;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                if (!(glyph[gx] >> gy & 1)) continue;
                int px = x0 + (i * adv + gx) * scale, py = y + gy * scale;
                for (int r = 0; r < scale; r++) {
                    memset(s_canvas + (size_t)(py + r) * EPD_W + px, 0, scale);
                }
            }
        }
    }
}

// The most characters of the font at `scale` that fit across the screen.
static int chars_per_line(int scale) {
    return (EPD_W + scale) / ((PIXEL_FONT_WIDTH + 1) * scale);
}

// Draw `text` centred on the row at `y`, in the largest pixel size up to
// `max_scale` that fits, cutting off what still does not fit at size 2.
static void draw_title(const char *text, int y, int max_scale) {
    int n = (int)strlen(text);
    int scale = max_scale;
    while (scale > 2 && n > chars_per_line(scale)) scale--;
    if (n > chars_per_line(scale)) n = chars_per_line(scale);
    y += (max_scale - scale) * PIXEL_FONT_HEIGHT / 2;
    draw_run(text, n, y, scale);
}

// Draw `text` from the row at `y` on up to two centred lines, broken at the
// last space that fits; what still does not fit is cut off.
static void draw_status(const char *text, int y, int scale) {
    const int max = chars_per_line(scale);
    int n = (int)strlen(text);
    int first = n;
    if (n > max) {
        first = max;
        while (first > 0 && text[first] != ' ') first--;
        if (first == 0) first = max;
    }
    draw_run(text, first, y, scale);
    const char *rest = text + first;
    while (*rest == ' ') rest++;
    int left = (int)strlen(rest);
    if (left) {
        draw_run(rest, left < max ? left : max, y + PIXEL_FONT_HEIGHT * scale + STATUS_LINE_GAP,
                 scale);
    }
}

// The character, still: the first frame of the animation in gray, with its
// black background as paper.
static void draw_character(void) {
    const uint8_t *cells = happy_anim_frames[0];
    for (int cy = 0; cy < HAPPY_ANIM_HEIGHT; cy++) {
        uint8_t *line = s_canvas + (size_t)(ANIM_Y + cy * ANIM_SCALE) * EPD_W + ANIM_X;
        for (int cx = 0; cx < HAPPY_ANIM_WIDTH; cx++) {
            uint8_t c = cells[cy * HAPPY_ANIM_WIDTH + cx];
            uint16_t be = happy_anim_palette[c];
            uint8_t v = c == 0 ? 255 : luma565((uint16_t)(be >> 8 | be << 8));
            for (int k = 0; k < ANIM_SCALE; k++) line[cx * ANIM_SCALE + k] = v;
        }
        for (int k = 1; k < ANIM_SCALE; k++) memcpy(line + k * EPD_W, line, ANIM_W);
    }
}

static void epd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATUS_SETTLE_MS))) {
        }
        char title[sizeof(s_title)];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const char *label = status_label(s_state);
        memcpy(title, s_title, sizeof(title));
        xSemaphoreGive(s_mutex);

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool redraw = !s_image_mode && (!s_status_drawn || label != s_drawn_label
                                        || strcmp(title, s_drawn_title) != 0);
        // A new screen after an image gets a full refresh too.
        bool full = !s_status_drawn || s_fast_refreshes >= FULL_REFRESH_EVERY;
        if (redraw) {
            memset(s_canvas, 255, CANVAS_BYTES);
            draw_title(title, TITLE_Y, TITLE_MAX_SCALE);
            draw_character();
            draw_status(label, STATUS_Y, STATUS_SCALE);
            epd_dither();
            // An image drawn during the refresh clears this again.
            s_status_drawn = true;
            s_drawn_label = label;
            memcpy(s_drawn_title, title, sizeof(s_drawn_title));
        }
        xSemaphoreGive(s_lock);
        if (redraw && epd_update(full) != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status_drawn = false;
            xSemaphoreGive(s_lock);
        }
        xSemaphoreGive(s_panel_lock);
        stack_monitor_poll(&stack);
    }
}

// ---- led_status.h -----------------------------------------------------------

static esp_err_t epd_init(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << EPD_PIN_DC | 1ULL << EPD_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t busy = {
        .pin_bit_mask = 1ULL << EPD_PIN_BUSY,
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err == ESP_OK) err = gpio_config(&busy);
    gpio_set_level(EPD_PIN_RST, 1);
    const spi_bus_config_t bus = {
        .sclk_io_num = EPD_PIN_SCLK,
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_CHUNK_BYTES,
    };
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = EPD_SPI_HZ,
        .spics_io_num = EPD_PIN_CS,
        .queue_size = 1,
    };
    if (err == ESP_OK) err = spi_bus_initialize(EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) err = spi_bus_add_device(EPD_HOST, &dev, &s_spi);
    return err;
}

bool led_status_init(void) {
    board_power_on();
    s_chunk = heap_caps_malloc(EPD_CHUNK_BYTES, MALLOC_CAP_DMA);
    s_canvas = heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    // What the panel shows at boot is unknown; the first refresh is full.
    s_shown = heap_caps_calloc(1, EPD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_err = heap_caps_malloc(2 * (EPD_W + 2) * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    s_panel_lock = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_mutex = xSemaphoreCreateMutex();
    if (!(s_chunk && s_canvas && s_frame && s_shown && s_err && s_panel_lock && s_lock
          && s_mutex)) {
        ESP_LOGE(TAG, "e-paper buffer alloc failed");
        return false;
    }
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "1.54 inch e-paper init failed: %s", esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(epd_task, "epd", 3072, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the e-paper task");
        return false;
    }
    s_ready = true;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LED status ready: Waveshare 1.54 inch %dx%d e-paper, 1 bit per pixel",
             EPD_W, EPD_H);
    return true;
}

void led_status_set_state(led_state_t state) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_state != state;
    s_state = state;
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

void led_status_set_title(const char *title) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_task);
}

bool led_status_display_info(int *width, int *height) {
    if (!s_ready) return false;
    *width = EPD_W;
    *height = EPD_H;
    return true;
}

int led_status_display_bits(void) {
    if (!s_ready) return 0;
    return 1;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > EPD_W || y + h > EPD_H) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)pixels;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_image_mode) {
        s_image_mode = true;
        s_status_drawn = false;
        memset(s_canvas, 255, CANVAS_BYTES);
    }
    for (int r = 0; r < h; r++) {
        uint8_t *line = s_canvas + (size_t)(y + r) * EPD_W + x;
        for (int i = 0; i < w; i++, src += 2) line[i] = luma565((uint16_t)(src[0] << 8 | src[1]));
    }
    s_image_dirty = true;
    xSemaphoreGive(s_lock);
    return true;
}

void led_status_draw_done(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_panel_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool show = s_image_mode && s_image_dirty;
    if (show) {
        s_image_dirty = false;
        epd_dither();
    }
    xSemaphoreGive(s_lock);
    if (show) epd_update(true);
    xSemaphoreGive(s_panel_lock);
}

void led_status_show_animation(void) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_image = s_image_mode;
    s_image_mode = false;
    s_image_dirty = false;
    xSemaphoreGive(s_lock);
    if (was_image) xTaskNotifyGive(s_task);
}
