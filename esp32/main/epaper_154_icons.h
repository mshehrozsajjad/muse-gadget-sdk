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

// The status bar icons on the 1.54 inch e-paper, drawn in black onto an 8-bit
// gray canvas `stride` pixels wide.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define ICON_WIFI_W     17
#define ICON_WIFI_H     13
#define ICON_WIFI_BARS  3
#define ICON_BATTERY_W  22
#define ICON_BATTERY_H  11
#define ICON_BATTERY_SEGMENTS 4
#define ICON_BOLT_W     7
#define ICON_BOLT_H     11

static inline void icon_dot(uint8_t *canvas, int stride, int x, int y) {
    canvas[(size_t)y * stride + x] = 0;
}

// Wi-Fi: a dot and three arcs fanning up from it. `bars` (1 to 3) arcs are
// solid and the rest dotted; 0 means offline: every arc dotted, no dot, and a
// cross in the corner.
static inline void icon_wifi(uint8_t *canvas, int stride, int x, int y, int bars) {
    // Squared inner and outer radii of the dot and the arcs.
    static const int rings[ICON_WIFI_BARS + 1][2] = {{0, 2}, {16, 30}, {49, 72}, {100, 132}};
    const int cx = x + ICON_WIFI_W / 2, cy = y + ICON_WIFI_H - 1;
    for (int py = y; py < y + ICON_WIFI_H; py++) {
        for (int px = x; px < x + ICON_WIFI_W; px++) {
            int dx = px - cx, dy = cy - py;
            if (abs(dx) > dy && !(dx == 0 && dy == 0)) continue;  // outside the fan
            int d2 = dx * dx + dy * dy;
            for (int ring = 0; ring <= ICON_WIFI_BARS; ring++) {
                if (d2 < rings[ring][0] || d2 > rings[ring][1]) continue;
                bool solid = bars > 0 && ring <= bars;
                bool dotted = (px + py) % 2 == 0 && ring > 0;
                if (solid || dotted) icon_dot(canvas, stride, px, py);
            }
        }
    }
    if (bars == 0) {
        for (int i = 0; i < 5; i++) {
            icon_dot(canvas, stride, x + ICON_WIFI_W - 5 + i, y + ICON_WIFI_H - 5 + i);
            icon_dot(canvas, stride, x + ICON_WIFI_W - 1 - i, y + ICON_WIFI_H - 5 + i);
        }
    }
}

// Battery: an outline with a terminal nub on the right and `segments` (0 to 4)
// filled cells inside, left to right.
static inline void icon_battery(uint8_t *canvas, int stride, int x, int y, int segments) {
    const int body_w = ICON_BATTERY_W - 2;
    for (int px = x; px < x + body_w; px++) {
        icon_dot(canvas, stride, px, y);
        icon_dot(canvas, stride, px, y + ICON_BATTERY_H - 1);
    }
    for (int py = y; py < y + ICON_BATTERY_H; py++) {
        icon_dot(canvas, stride, x, py);
        icon_dot(canvas, stride, x + body_w - 1, py);
    }
    for (int py = y + 3; py < y + ICON_BATTERY_H - 3; py++) {
        icon_dot(canvas, stride, x + body_w, py);
        icon_dot(canvas, stride, x + body_w + 1, py);
    }
    for (int s = 0; s < segments && s < ICON_BATTERY_SEGMENTS; s++) {
        for (int py = y + 2; py < y + ICON_BATTERY_H - 2; py++) {
            for (int px = 0; px < 3; px++) icon_dot(canvas, stride, x + 2 + s * 4 + px, py);
        }
    }
}

// A lightning bolt: on USB power from a computer (beside the battery).
static inline void icon_bolt(uint8_t *canvas, int stride, int x, int y) {
    static const uint8_t rows[ICON_BOLT_H] = {
        0x0C, 0x18, 0x18, 0x30, 0x3E, 0x7C, 0x0C, 0x18, 0x18, 0x30, 0x20,
    };
    for (int r = 0; r < ICON_BOLT_H; r++) {
        for (int c = 0; c < ICON_BOLT_W; c++) {
            if (rows[r] >> (ICON_BOLT_W - 1 - c) & 1) icon_dot(canvas, stride, x + c, y + r);
        }
    }
}
