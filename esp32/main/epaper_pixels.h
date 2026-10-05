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

// Black and white e-paper pixels, shared by the reTerminal driver
// (epaper_status.c) and the Waveshare 1.54 inch one (epaper_154_status.c).
// Host-tested through tests/link_epaper_status_harness.c.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Gray level of a native RGB565 pixel, 0 (black) to 255 (white).
static inline uint8_t luma565(uint16_t px) {
    int r = (px >> 11) & 0x1F, g = (px >> 5) & 0x3F, b = px & 0x1F;
    r = r << 3 | r >> 2;
    g = g << 2 | g >> 4;
    b = b << 3 | b >> 2;
    return (uint8_t)((77 * r + 150 * g + 29 * b + 128) >> 8);
}

// Dither w x h gray pixels to 1 bit with Floyd-Steinberg, packed 8 to a byte,
// most significant bit leftmost, 1 for white (the controller's order). `w`
// is a multiple of 8. `err` holds 2 * (w + 2) values.
static inline void dither_frame(const uint8_t *gray, uint8_t *bits, int w, int h, int16_t *err) {
    int16_t *cur = err, *next = err + w + 2;
    memset(err, 0, 2 * (size_t)(w + 2) * sizeof(*err));
    for (int y = 0; y < h; y++) {
        const uint8_t *row = gray + (size_t)y * w;
        uint8_t *out = bits + (size_t)y * (w / 8);
        memset(next, 0, (size_t)(w + 2) * sizeof(*next));
        for (int x = 0; x < w; x += 8) {
            uint8_t byte = 0;
            for (int k = 0; k < 8; k++) {
                // cur and next are offset by one so that x - 1 stays in range.
                int i = x + k;
                int v = row[i] + cur[i + 1];
                bool white = v >= 128;
                int e = v - (white ? 255 : 0);
                cur[i + 2] += (int16_t)(e * 7 / 16);
                next[i] += (int16_t)(e * 3 / 16);
                next[i + 1] += (int16_t)(e * 5 / 16);
                next[i + 2] += (int16_t)(e / 16);
                byte = (uint8_t)(byte << 1 | white);
            }
            out[x / 8] = byte;
        }
        int16_t *t = cur;
        cur = next;
        next = t;
    }
}
