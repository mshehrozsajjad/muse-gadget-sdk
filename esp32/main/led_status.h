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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LED_STATE_BOOT,
    LED_STATE_SETUP_IDLE,
    LED_STATE_BLE_ADVERTISING,
    LED_STATE_BLE_CONNECTED,
    LED_STATE_PAIRING_CONFIRM_REQUIRED,
    LED_STATE_WIFI_CONNECTING,
    LED_STATE_WIFI_CONNECTED,
    LED_STATE_AUTH_OK,
    LED_STATE_VM_SWITCHING,
    LED_STATE_VM_OK,
    LED_STATE_WS_CONNECTED,
    LED_STATE_WS_DISCONNECTED,
    LED_STATE_UNPAIRED,
    LED_STATE_ERROR,
} led_state_t;

bool led_status_init(void);
void led_status_set_state(led_state_t state);
// Show a short title (the agent's name) on backends with a display; NULL or ""
// clears it. Other backends ignore it.
void led_status_set_title(const char *title);
// The button's reset countdown: seconds left (1 to 5) over whatever is shown,
// 0 to take it away. Backends without a screen ignore it.
void led_status_show_reset_countdown(int seconds_left);
// Before deep sleep: shows the sleep screen (returns once it's up) and gets
// the board ready to sleep, its rails off and its power held. False if this
// backend can't, and the device then stays awake.
bool led_status_prepare_deep_sleep(void);

// Display backends only: the screen size in pixels. Returns false without a
// display.
bool led_status_display_info(int *width, int *height);
// Bits per pixel the screen shows: 16 for the RGB565 colour LCDs, 4 for
// six-colour e-paper (the reTerminal E1002's Spectra 6), 1 for black and
// white e-paper, 0 without a display.
int led_status_display_bits(void);
// Draw w x h pixels at (x, y). `pixels` holds RGB565, 2 bytes each with the
// high byte first, left to right and top to bottom; at most 23 full rows'
// worth per call. The first call replaces the animation and title until
// led_status_show_animation(); the status bars and dot stay on top. E-paper
// dithers colour to its inks (black and white, or the E1002's six), and shows
// it only at led_status_draw_done().
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels);
// The image is complete: e-paper refreshes now, and returns once it has
// (a second or two in black and white, about 30 s in six colours). LCDs have
// shown each draw already.
void led_status_draw_done(void);
// Clear the image and bring back the animation and title.
void led_status_show_animation(void);

// What the voice chat is doing, shown on the LED ring in place of the
// connection status until it is back to idle. An error flashes red three
// times and then returns to idle by itself.
typedef enum {
    LED_VOICE_IDLE,
    LED_VOICE_LISTENING,     // blue level meter (led_status_set_level)
    LED_VOICE_TRANSCRIBING,  // amber comet
    LED_VOICE_THINKING,      // purple comet: waiting for the agent
    LED_VOICE_BUFFERING,     // green comet: the reply is being synthesised
    LED_VOICE_SPEAKING,      // green breathing
    LED_VOICE_ERROR,
} led_voice_t;

void led_status_set_voice(led_voice_t voice);
// Microphone level for LED_VOICE_LISTENING, 0 to 1.
void led_status_set_level(float level);
// Show the speaker volume (0 to 100) on the ring for a moment, over whatever
// it shows.
void led_status_show_volume(int percent);

// The longest card text, in bytes, wrapped lines included.
#define LED_STATUS_CARD_MAX 1536

// The reply card's page in characters, for backends that show a reply's text
// (muse_hatch_wrap wraps to its width). False without a card.
bool led_status_reply_page(int *cols, int *lines);
// Show a reply in place of the status screen until the next voice turn, or a
// while after this one: all of it, wrapped to the page's width, lines
// separated by '\n'. Longer than a page, it scrolls (led_status_scroll_card).
// Called again as the reply grows, it keeps the page shown. Backends without
// a card ignore it.
void led_status_show_reply(const char *lines);
// As led_status_show_reply, for a note Muse sent on its own: from its first
// page, and with no timeout, until led_status_dismiss_card() or the next
// voice turn.
void led_status_show_note(const char *lines);
// Turn the card's page: forward for positive `pages`, back for negative,
// stopping at either end. False if no card is showing.
bool led_status_scroll_card(int pages);
// Take away a reply card or note, back to the status screen. True if one was
// showing. Backends without a card return false.
bool led_status_dismiss_card(void);

// What the menu has switched off: shown in place of the connection status
// ("Wi-Fi off", or "Mic off" where it would say connected). Backends without
// a screen ignore it.
void led_status_set_switches(bool wifi_on, bool mic_on);

// A menu row: its name on the left, its value (or "") on the right.
#define LED_MENU_ROWS_MAX 8
typedef struct {
    char label[20];
    char value[8];
} led_menu_row_t;

// Show a menu over everything but the reset countdown: `count` rows, the
// `selected` one highlighted, its value marked for adjusting when
// `adjusting`. Called again, it redraws only what changed. NULL (or a count
// of 0) closes it. Backends without a display ignore it.
void led_status_show_menu(const led_menu_row_t *rows, int count, int selected, bool adjusting);
