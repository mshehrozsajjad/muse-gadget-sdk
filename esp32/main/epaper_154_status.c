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
// a still status screen (Wi-Fi and battery icons, the agent's name, the
// character and the status text), redrawn only when one of them changes. The
// character is the SDK's own (avatar/muse_pixel.c), in the pose for the
// state, with a short one-time animation on entering some states
// (epaper_154_animation.h).
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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "battery_154.h"
#include "climate_154.h"
#include "epaper_154_icons.h"
#include "epaper_154_animation.h"
#include "epaper_pixels.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_pixel.h"
#if CONFIG_HOMEHUB_VOICE
#include "muse_text.h"
#endif
#include "pixel_font.h"
#include "sdkconfig.h"
#include "stack_monitor.h"
#include "wifi_mgr.h"

static const char *TAG = "link.led";

// ---- Board ------------------------------------------------------------------

#define BOARD_PIN_VBAT_HOLD 17  // high keeps the battery switched on
#define BOARD_PIN_EPD_POWER 6   // low powers the panel
// Low powers the audio circuit, and the SHTC3 with it: the sensor doesn't
// answer until this is on (voice_board_waveshare_epaper_154.c drives it too).
#define BOARD_PIN_AUDIO_POWER 42

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
// connecting costs one refresh. A voice change waits much less: "Listening"
// has to show while the button is still held.
#define STATUS_SETTLE_MS 1500
#define VOICE_SETTLE_MS  30
// Fast refreshes leave a faint ghost of the old picture that builds up; a full
// refresh clears it but flashes. So the clean waits for a calm screen (no
// voice turn, reply card or animation under way) once this many fast ones
// have built up, and comes regardless only after the second number.
#define CLEAN_AFTER_FAST   20
#define CLEAN_FORCE_FAST   40
// How often the icons are checked. They change the screen only when a whole
// bar or battery cell does, and only once past a margin, so that a reading
// sitting on a threshold doesn't keep refreshing it.
#define ICON_POLL_MS        60000
#define WIFI_MARGIN_DB      3
#define BATTERY_MARGIN_PCT  3
#define CLIMATE_MARGIN_C    0.8f
#define CLIMATE_MARGIN_RH   3.0f

// Status screen: Wi-Fi and battery icons along the top, the title below them,
// the character in the middle, up to two lines of status text at the bottom.
#define BAR_Y            1
#define BAR_MARGIN       4
#define TITLE_Y          18
#define TITLE_MAX_SCALE  2
#define CHARACTER_SIZE   112
#define CHARACTER_X      ((EPD_W - CHARACTER_SIZE) / 2)
#define CHARACTER_Y      44
#define STATUS_Y         162
#define STATUS_SCALE     2
#define STATUS_LINE_GAP  4

// Reply card: the status bar, then the reply's opening page, left-aligned:
// in the status text's size (2x) when the reply fits, or compact (1.5x) to
// fit nearly twice as much when it doesn't.
#define REPLY_X          4
#define REPLY_Y          20
#define REPLY_MAX        512

typedef struct {
    int halves;  // font pixel size in half pixels
    int cols, lines;
    int line_h;  // in pixels
} reply_layout_t;

static const reply_layout_t s_layout_normal = {
    .halves = STATUS_SCALE * 2,
    .cols = 16,  // 12 px a character
    .lines = 9,
    .line_h = PIXEL_FONT_HEIGHT * STATUS_SCALE + STATUS_LINE_GAP,  // 20 px
};
static const reply_layout_t s_layout_compact = {
    .halves = 3,
    .cols = 21,  // 9 px a character
    .lines = 12,
    .line_h = PIXEL_FONT_HEIGHT * 3 / 2 + 3,  // 15 px
};
// The card gives way to the status screen this long after the turn ends.
#define REPLY_SHOW_MS    60000

// Switch the battery hold, the panel and the audio rail (for the sensor) on,
// and keep the first two through light sleep.
static void board_power_on(void) {
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << BOARD_PIN_VBAT_HOLD | 1ULL << BOARD_PIN_EPD_POWER
                        | 1ULL << BOARD_PIN_AUDIO_POWER,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_PIN_VBAT_HOLD, 1);
    gpio_set_level(BOARD_PIN_EPD_POWER, 0);
    gpio_set_level(BOARD_PIN_AUDIO_POWER, 0);
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
static int s_fast_refreshes;  // since the last full refresh

// Guards s_canvas and the drawn-state below.
static SemaphoreHandle_t s_lock;
static bool s_image_mode;    // an image replaces the status screen
static bool s_image_dirty;   // drawn since the last refresh
static bool s_animation_hidden; // cancel even if an image is shown and cleared between frames
static bool s_status_drawn;  // the status screen shows s_drawn_*
static bool s_redraw_pending; // a dropped frame: redraw, but no need to flash
static const char *s_drawn_label;
static char s_drawn_title[48];
static int s_drawn_wifi, s_drawn_battery;

// Temperature and humidity as shown, whole degrees and percent; valid false
// when the sensor doesn't answer, which leaves them off the bar.
typedef struct {
    bool valid;
    int celsius, humidity;
} climate_t;
static climate_t s_drawn_climate;
static muse_mode_t s_drawn_mode;
static char s_drawn_reply[REPLY_MAX];
static bool s_drawn_reply_more, s_drawn_reply_compact;

// Requested by led_status_set_state(), _set_title() and _set_voice();
// guarded by s_mutex. While a voice turn runs, its state stands in for the
// connection's in the status text and the pose.
static SemaphoreHandle_t s_mutex;
static led_state_t s_state = LED_STATE_BOOT;
static led_voice_t s_voice = LED_VOICE_IDLE;
static bool s_voice_changed;  // settle quickly: the change is a voice one
static uint32_t s_pose_generation; // captures rapid leave/re-enter during a panel refresh
static int s_reset_left;         // the button's reset countdown, 0 for none
static char s_reply[REPLY_MAX];  // the reply card's page, "" for none
static bool s_reply_more;        // the reply goes on past the page
static bool s_reply_compact;     // laid out with s_layout_compact
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

// Draw the first `n` bytes of `text` in black, each font pixel `halves` / 2
// screen pixels wide (4 for 2x, 3 for 1.5x, where font pixels alternate
// between 2 and 1), from (x0, y). Bytes outside printable ASCII show as '?'.
static void draw_run_halves(const char *text, int n, int x0, int y, int halves) {
    const int adv = PIXEL_FONT_WIDTH + 1;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < PIXEL_FONT_FIRST || ch > PIXEL_FONT_LAST) ch = '?';
        const uint8_t *glyph = pixel_font[ch - PIXEL_FONT_FIRST];
        for (int gx = 0; gx < PIXEL_FONT_WIDTH; gx++) {
            int col = i * adv + gx;
            int px = x0 + col * halves / 2, pw = x0 + (col + 1) * halves / 2 - px;
            for (int gy = 0; gy < PIXEL_FONT_HEIGHT; gy++) {
                if (!(glyph[gx] >> gy & 1)) continue;
                int py = y + gy * halves / 2, ph = y + (gy + 1) * halves / 2 - py;
                for (int r = 0; r < ph; r++) {
                    memset(s_canvas + (size_t)(py + r) * EPD_W + px, 0, pw);
                }
            }
        }
    }
}

// As draw_run_halves, at a whole pixel size `scale`.
static void draw_run(const char *text, int n, int x0, int y, int scale) {
    draw_run_halves(text, n, x0, y, scale * 2);
}

// The most characters of the font at `scale` that fit across the screen.
static int chars_per_line(int scale) {
    return (EPD_W + scale) / ((PIXEL_FONT_WIDTH + 1) * scale);
}

// Where a run of `n` characters at `scale` starts to sit centred.
static int centred_x(int n, int scale) {
    return (EPD_W - (n * (PIXEL_FONT_WIDTH + 1) * scale - scale)) / 2;
}

// Draw `text` centred on the row at `y`, in the largest pixel size up to
// `max_scale` that fits, cutting off what still does not fit at size 2.
static void draw_title(const char *text, int y, int max_scale) {
    int n = (int)strlen(text);
    int scale = max_scale;
    while (scale > 2 && n > chars_per_line(scale)) scale--;
    if (n > chars_per_line(scale)) n = chars_per_line(scale);
    y += (max_scale - scale) * PIXEL_FONT_HEIGHT / 2;
    draw_run(text, n, centred_x(n, scale), y, scale);
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
    draw_run(text, first, centred_x(first, scale), y, scale);
    const char *rest = text + first;
    while (*rest == ' ') rest++;
    int left = (int)strlen(rest);
    if (left) {
        int n2 = left < max ? left : max;
        draw_run(rest, n2, centred_x(n2, scale), y + PIXEL_FONT_HEIGHT * scale + STATUS_LINE_GAP,
                 scale);
    }
}

// The status text for the reset countdown, `left` seconds from 1 to 5.
static const char *countdown_label(int left) {
    static const char *const labels[] = {
        "Keep holding to reset 1", "Keep holding to reset 2", "Keep holding to reset 3",
        "Keep holding to reset 4", "Keep holding to reset 5",
    };
    if (left < 1) left = 1;
    if (left > 5) left = 5;
    return labels[left - 1];
}

// The status text for a voice turn's state, NULL when idle.
static const char *voice_label(led_voice_t voice) {
    switch (voice) {
        case LED_VOICE_LISTENING:    return "Listening";
        case LED_VOICE_TRANSCRIBING:
        case LED_VOICE_THINKING:
        case LED_VOICE_BUFFERING:    return "Thinking";
        case LED_VOICE_SPEAKING:     return "Replying";
        case LED_VOICE_ERROR:        return "Didn't work, try again";
        case LED_VOICE_IDLE:         break;
    }
    return NULL;
}

// The character's pose for a voice turn's state.
static muse_mode_t voice_mode(led_voice_t voice) {
    switch (voice) {
        case LED_VOICE_LISTENING: return MUSE_MODE_LISTENING;
        case LED_VOICE_SPEAKING:  return MUSE_MODE_SPEAKING;
        case LED_VOICE_ERROR:     return MUSE_MODE_ERROR;
        default:                  return MUSE_MODE_THINKING;
    }
}

#if CONFIG_HOMEHUB_VOICE
// One line of reply text in the pixel font's ASCII: the stand-ins the
// caption wrapping counted (curly quotes straight, emoji dropped), '?' for
// what has none. Returns the length, at most `cap` - 1.
static int reply_line_ascii(const char *line, size_t bytes, char *out, int cap) {
    int n = 0;
    for (size_t i = 0; i < bytes && n < cap - 1;) {
        size_t len = 1;
        char shown[4];
        int w = muse_text_ascii(line + i, &len, shown);
        if (w < 0) {
            out[n++] = len == 1 ? line[i] : '?';
        } else {
            for (int k = 0; k < w && n < cap - 1; k++) out[n++] = shown[k];
        }
        i += len ? len : 1;
    }
    out[n] = '\0';
    return n;
}

// The reply's page, line by line, with "..." closing the last line when the
// reply goes on.
static void draw_reply(const char *page, bool more, const reply_layout_t *layout) {
    const int cols = layout->cols;
    char line[64];
    for (int row = 0; row < layout->lines && *page; row++) {
        const char *end = strchr(page, '\n');
        size_t bytes = end ? (size_t)(end - page) : strlen(page);
        int n = reply_line_ascii(page, bytes, line, sizeof(line));
        page = end ? end + 1 : page + bytes;
        bool last = row == layout->lines - 1 || !*page;
        if (last && more) {
            while (n > cols - 3 || (n && line[n - 1] == ' ')) n--;
            memcpy(line + n, "...", 4);
            n += 3;
        }
        draw_run_halves(line, n < cols ? n : cols, REPLY_X, REPLY_Y + row * layout->line_h,
                        layout->halves);
    }
}
#endif

// The character's pose for a connection state.
static muse_mode_t character_mode(led_state_t state) {
    switch (state) {
        case LED_STATE_BOOT:              return MUSE_MODE_BOOT;
        case LED_STATE_WIFI_CONNECTING:
        case LED_STATE_WIFI_CONNECTED:
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK:
        case LED_STATE_WS_DISCONNECTED:   return MUSE_MODE_THINKING;
        case LED_STATE_UNPAIRED:
        case LED_STATE_ERROR:             return MUSE_MODE_ERROR;
        default:                          return MUSE_MODE_IDLE;
    }
}

// The character in `mode`, still, in gray, with its black background as
// paper. The renderer eases its colours from one mode to the next over
// time, so a few frames settle the palette while holding the chosen key pose.
static void draw_character(muse_mode_t mode, float mode_t) {
    static uint16_t row[CHARACTER_SIZE];
    muse_pose_t pose = {.mode = mode};
    for (int i = 0; i < 10; i++) {
        pose.t += 0.2f;
        pose.mode_t = mode_t;
        muse_pixel_render(&pose);
    }
    for (int y = 0; y < CHARACTER_SIZE; y++) {
        muse_pixel_scale(row, CHARACTER_SIZE, 0, CHARACTER_SIZE - 1, y, y);
        uint8_t *line = s_canvas + (size_t)(CHARACTER_Y + y) * EPD_W + CHARACTER_X;
        for (int x = 0; x < CHARACTER_SIZE; x++) line[x] = row[x] == 0 ? 255 : luma565(row[x]);
    }
}

// Signal bars, 1 to 3, for an RSSI in dBm.
static int wifi_bars_at(int rssi) {
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
}

// Wi-Fi signal bars now, 0 when not connected. `previous` stands while the
// RSSI is within the margin of its range.
static int wifi_bars(int previous) {
    // Boot now draws before wifi_mgr_init(). The RSSI API does not guard
    // against an uninitialized Wi-Fi task in ESP-IDF 6.0.1.
    if (!wifi_mgr_is_connected()) return 0;
    int rssi;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) return 0;
    int low = wifi_bars_at(rssi - WIFI_MARGIN_DB), high = wifi_bars_at(rssi + WIFI_MARGIN_DB);
    if (previous >= low && previous <= high) return previous;
    return wifi_bars_at(rssi);
}

// Filled battery cells, 0 to 4, for a charge in percent: each cell is 25 %,
// centred on its share.
static int battery_cells_at(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (percent + 12) / 25;
}

// Battery cells now. `previous` (-1 at first) stands while the charge is
// within the margin of its range.
static int battery_cells(int previous) {
    int mv = battery_154_millivolts();
    int percent = battery_154_percent(mv);
    int low = battery_cells_at(percent - BATTERY_MARGIN_PCT);
    int high = battery_cells_at(percent + BATTERY_MARGIN_PCT);
    if (previous >= low && previous <= high) return previous;
    int cells = battery_cells_at(percent);
    ESP_LOGI(TAG, "battery %d mV, %d%%: %d of %d cells", mv, percent, cells,
             ICON_BATTERY_SEGMENTS);
    return cells;
}

// Temperature and humidity now, in whole units. `previous` stands while the
// reading is within the margin of it, so small drifts don't redraw the bar.
static climate_t climate_now(climate_t previous) {
    float celsius, humidity;
    if (!climate_154_read(&celsius, &humidity)) return (climate_t){0};
    climate_t now = {
        .valid = true,
        .celsius = (int)(celsius + (celsius < 0 ? -0.5f : 0.5f)),
        .humidity = (int)(humidity + 0.5f),
    };
    if (previous.valid && fabsf(celsius - previous.celsius) < CLIMATE_MARGIN_C
        && fabsf(humidity - previous.humidity) < CLIMATE_MARGIN_RH) {
        return previous;
    }
    return now;
}

// A degree sign: a 4 x 4 ring, its top at `y`.
static void draw_degree(int x, int y) {
    static const uint8_t ring[4] = {0x6, 0x9, 0x9, 0x6};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (ring[r] >> (3 - c) & 1) s_canvas[(size_t)(y + r) * EPD_W + x + c] = 0;
        }
    }
}

// "23°C 45%", centred between the icons, in the compact text's size.
static void draw_climate(climate_t climate) {
    if (!climate.valid) return;
    const int halves = 3, adv = (PIXEL_FONT_WIDTH + 1) * halves / 2, degree_w = 6;
    char temp[8], rest[12];
    int nt = snprintf(temp, sizeof(temp), "%d", climate.celsius);
    int nr = snprintf(rest, sizeof(rest), "C %d%%", climate.humidity);
    int width = (nt + nr) * adv + degree_w;
    int x = (EPD_W - width) / 2, y = BAR_Y + 1;
    draw_run_halves(temp, nt, x, y, halves);
    draw_degree(x + nt * adv, y);
    draw_run_halves(rest, nr, x + nt * adv + degree_w, y, halves);
}

static void draw_status_bar(int wifi, int battery, climate_t climate) {
    icon_wifi(s_canvas, EPD_W, BAR_MARGIN, BAR_Y, wifi);
    draw_climate(climate);
    icon_battery(s_canvas, EPD_W, EPD_W - BAR_MARGIN - ICON_BATTERY_W, BAR_Y + 1, battery);
}

static void epd_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    int wifi = 0, battery = -1;
    climate_t climate = {0};
    int64_t card_until = 0;  // when the reply card goes, 0 while it stays
    epaper_animation_t animation = {0};
#if CONFIG_HOMEHUB_EPAPER_154_ANIMATIONS
    const bool animate = true;
    const int hold_ms = CONFIG_HOMEHUB_EPAPER_154_ANIMATION_HOLD_MS;
#else
    const bool animate = false;
    const int hold_ms = 0;
#endif
    for (;;) {
        // A status change, time to check the icons, or the card's time is up.
        int wait_ms = animation.initialized ? ICON_POLL_MS : 0;
        if (animation.next_ms) {
            int64_t left_ms = animation.next_ms - esp_timer_get_time() / 1000;
            if (left_ms < wait_ms) wait_ms = left_ms < 0 ? 0 : (int)left_ms;
        }
        if (card_until) {
            int64_t left_ms = (card_until - esp_timer_get_time()) / 1000;
            wait_ms = left_ms < 0 ? 0 : left_ms < wait_ms ? (int)left_ms : wait_ms;
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)) && animation.initialized) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            int settle_ms = animation.next_ms ? 0 :
                            s_voice_changed ? VOICE_SETTLE_MS : STATUS_SETTLE_MS;
            s_voice_changed = false;
            xSemaphoreGive(s_mutex);
            while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(settle_ms))) {
                // A voice event arriving during connection debounce must not
                // wait out another 1.5-second connection window.
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_voice_changed && settle_ms) settle_ms = VOICE_SETTLE_MS;
                s_voice_changed = false;
                xSemaphoreGive(s_mutex);
            }
        }
        wifi = wifi_bars(wifi);
        battery = battery_cells(battery);
        climate = climate_now(climate);
        char title[sizeof(s_title)];
        static char reply[REPLY_MAX];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        // The card's time starts once the turn is over.
        if (!s_reply[0] || s_voice != LED_VOICE_IDLE) {
            card_until = 0;
        } else if (!card_until) {
            card_until = esp_timer_get_time() + REPLY_SHOW_MS * 1000LL;
        } else if (esp_timer_get_time() >= card_until) {
            s_reply[0] = '\0';
            card_until = 0;
        }
        memcpy(reply, s_reply, sizeof(reply));
        bool reply_more = s_reply_more;
        bool reply_compact = s_reply_compact;
        const char *label = voice_label(s_voice);
        muse_mode_t mode = label ? voice_mode(s_voice) : character_mode(s_state);
        uint32_t generation = s_pose_generation;
        bool voice_idle = s_voice == LED_VOICE_IDLE;
        int reset_left = s_reset_left;
        bool boot_allowed = !reset_left && s_voice == LED_VOICE_IDLE && s_state != LED_STATE_ERROR &&
                            s_state != LED_STATE_UNPAIRED &&
                            s_state != LED_STATE_PAIRING_CONFIRM_REQUIRED && !reply[0];
        if (!label) label = status_label(s_state);
        memcpy(title, s_title, sizeof(title));
        xSemaphoreGive(s_mutex);
        if (reset_left) {
            // Over everything, the card included, until the button is let go.
            label = countdown_label(reset_left);
            mode = MUSE_MODE_ERROR;
            reply[0] = '\0';
        }

        xSemaphoreTake(s_panel_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool holding_boot = epaper_animation_keep_boot(&animation, animate,
                            boot_allowed && !s_image_mode && !s_animation_hidden,
                            esp_timer_get_time() / 1000);
        if (holding_boot) {
            mode = MUSE_MODE_BOOT;
            label = status_label(LED_STATE_BOOT);
            if (animation.initialized) generation = animation.generation;
        }
        if (s_animation_hidden) {
            epaper_animation_prepare(&animation, generation, mode, false, animate, 0);
            s_animation_hidden = false;
        }
        bool frame_due = epaper_animation_prepare(&animation, generation, mode,
                         !s_image_mode && !reply[0], animate, esp_timer_get_time() / 1000);
        bool redraw = !s_image_mode && (frame_due || s_redraw_pending || !s_status_drawn
                                        || label != s_drawn_label
                                        || strcmp(title, s_drawn_title) != 0
                                        || mode != s_drawn_mode || wifi != s_drawn_wifi
                                        || battery != s_drawn_battery
                                        || memcmp(&climate, &s_drawn_climate, sizeof(climate)) != 0
                                        || strcmp(reply, s_drawn_reply) != 0
                                        || reply_more != s_drawn_reply_more
                                        || reply_compact != s_drawn_reply_compact);
        // The first screen, and the first after an image or a failed refresh,
        // is full. Otherwise the ghosting is cleaned when the screen is calm.
        bool animating = animation.last && animation.frame < animation.last;
        bool calm = voice_idle && !reply[0] && !animating && !reset_left;
        bool full = !s_status_drawn || s_fast_refreshes >= CLEAN_FORCE_FAST
                    || (calm && s_fast_refreshes >= CLEAN_AFTER_FAST);
        if (redraw) {
            memset(s_canvas, 255, CANVAS_BYTES);
            draw_status_bar(wifi, battery, climate);
#if CONFIG_HOMEHUB_VOICE
            if (reply[0]) {
                draw_reply(reply, reply_more, reply_compact ? &s_layout_compact : &s_layout_normal);
            } else
#endif
            {
                draw_title(title, TITLE_Y, TITLE_MAX_SCALE);
                draw_character(mode, epaper_animation_pose_time(&animation));
                draw_status(label, STATUS_Y, STATUS_SCALE);
            }
            epd_dither();
            // An image drawn during the refresh clears this again.
            s_status_drawn = true;
            s_redraw_pending = false;
            s_drawn_label = label;
            memcpy(s_drawn_title, title, sizeof(s_drawn_title));
            s_drawn_wifi = wifi;
            s_drawn_battery = battery;
            s_drawn_climate = climate;
            s_drawn_mode = mode;
            memcpy(s_drawn_reply, reply, sizeof(s_drawn_reply));
            s_drawn_reply_more = reply_more;
            s_drawn_reply_compact = reply_compact;
        }
        xSemaphoreGive(s_lock);
        // Recheck after rendering: never start a refresh for a superseded
        // action. A refresh already in flight cannot be cancelled by this panel.
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool superseded = holding_boot ?
            (s_voice != LED_VOICE_IDLE || s_state == LED_STATE_ERROR ||
             s_state == LED_STATE_UNPAIRED || s_state == LED_STATE_PAIRING_CONFIRM_REQUIRED ||
             s_reply[0]) : generation != s_pose_generation;
        xSemaphoreGive(s_mutex);
        if (redraw && superseded) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_redraw_pending = true;
            xSemaphoreGive(s_lock);
            xTaskNotifyGive(s_task);
        } else if (redraw) {
            bool success = epd_update(full) == ESP_OK;
            epaper_animation_presented(&animation, esp_timer_get_time() / 1000, success, hold_ms);
            if (!success) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status_drawn = false;
                xSemaphoreGive(s_lock);
            }
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
    muse_pixel_set_size(CHARACTER_SIZE);
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
    if (!battery_154_init()) ESP_LOGW(TAG, "no battery level: the icon stays empty");
    if (!climate_154_init()) ESP_LOGW(TAG, "no temperature or humidity on the bar");
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
    if (changed && s_voice == LED_VOICE_IDLE) s_pose_generation++;
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

void led_status_show_reset_countdown(int seconds_left) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_reset_left != seconds_left;
    s_reset_left = seconds_left;
    if (changed) {
        s_voice_changed = true;  // show it at once, as a voice change would
        s_pose_generation++;
    }
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
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
        s_animation_hidden = true;
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

// ---- Voice -----------------------------------------------------------------

#if CONFIG_HOMEHUB_VOICE
void led_status_set_voice(led_voice_t voice) {
    if (!s_ready) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_voice != voice;  // the voice task repeats states
    s_voice = voice;
    if (changed) {
        s_voice_changed = true;
        s_pose_generation++;
    }
    if (voice == LED_VOICE_LISTENING) s_reply[0] = '\0';  // a new turn: the card goes
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

bool led_status_reply_page(bool compact, int *cols, int *lines) {
    const reply_layout_t *layout = compact ? &s_layout_compact : &s_layout_normal;
    *cols = layout->cols;
    *lines = layout->lines;
    return true;
}

void led_status_show_reply(const char *page, bool more, bool compact) {
    if (!s_ready || !page) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = strcmp(s_reply, page) != 0 || s_reply_more != more || s_reply_compact != compact;
    snprintf(s_reply, sizeof(s_reply), "%s", page);
    s_reply_more = more;
    s_reply_compact = compact;
    if (changed) {
        s_voice_changed = true;
        s_pose_generation++;
    }
    xSemaphoreGive(s_mutex);
    if (changed) xTaskNotifyGive(s_task);
}

// No live level meter: e-paper can't keep up with it.
void led_status_set_level(float level) {
    (void)level;
}

// Nothing turns the volume on the device yet (no dial), so nothing to show.
void led_status_show_volume(int percent) {
    (void)percent;
}
#endif
