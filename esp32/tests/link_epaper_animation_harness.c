// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "epaper_154_animation.h"
#include "epaper_pixels.h"

static void scheduler(void) {
    epaper_animation_t a = {0};
    assert(epaper_animation_keep_boot(&a, true, true, 0));
    assert(!epaper_animation_keep_boot(&a, true, false, 0));
    assert(!epaper_animation_keep_boot(&a, false, true, 0));
    assert(epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 0));
    assert(a.frame == 0 && a.last == 2);
    // Nothing advances while the panel is busy, even after a long refresh.
    assert(!epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 5000));
    epaper_animation_presented(&a, 5000, true, 450);
    assert(!epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 5449));
    assert(epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 5450));
    assert(a.frame == 1);
    assert(epaper_animation_keep_boot(&a, true, true, 5450));
    assert(!epaper_animation_keep_boot(&a, true, false, 5450));
    epaper_animation_presented(&a, 6000, true, 450);
    // An unrelated screen update must not push back the next frame.
    epaper_animation_presented(&a, 6200, true, 450);
    assert(a.next_ms == 6450);
    assert(epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 9000));
    assert(a.frame == 2);
    epaper_animation_presented(&a, 9500, true, 450);
    assert(a.next_ms == 9950);
    assert(epaper_animation_keep_boot(&a, true, true, 9949));
    assert(!epaper_animation_keep_boot(&a, true, true, 9950));
    assert(!epaper_animation_prepare(&a, 0, MUSE_MODE_BOOT, true, true, 100000));
    assert(a.frame == 2 && a.next_ms == 0); // no fourth frame or replay

    // The shorter listening hold still starts after the physical refresh.
    epaper_animation_t fast = {0};
    epaper_animation_prepare(&fast, 1, MUSE_MODE_LISTENING, true, true, 0);
    epaper_animation_presented(&fast, 574, true, 100);
    assert(!epaper_animation_prepare(&fast, 1, MUSE_MODE_LISTENING, true, true, 673));
    assert(epaper_animation_prepare(&fast, 1, MUSE_MODE_LISTENING, true, true, 674));

    // Listening interrupts boot; release interrupts listening immediately.
    assert(epaper_animation_prepare(&a, 1, MUSE_MODE_LISTENING, true, true, 100001));
    assert(a.frame == 0);
    epaper_animation_presented(&a, 101000, true, 450);
    assert(epaper_animation_prepare(&a, 2, MUSE_MODE_THINKING, true, true, 101001));
    assert(a.last == 0 && a.next_ms == 0);

    // A rapid release/repress during refresh is a NEW listening action.
    assert(epaper_animation_prepare(&a, 3, MUSE_MODE_LISTENING, true, true, 102000));
    epaper_animation_presented(&a, 103000, true, 450);
    assert(epaper_animation_prepare(&a, 5, MUSE_MODE_LISTENING, true, true, 103001));
    assert(a.frame == 0 && a.next_ms == 0);

    // Reply cards and external images cancel; uncovering does not replay.
    assert(!epaper_animation_prepare(&a, 5, MUSE_MODE_LISTENING, false, true, 104000));
    assert(a.frame == a.last && a.next_ms == 0);
    assert(!epaper_animation_prepare(&a, 5, MUSE_MODE_LISTENING, true, true, 105000));
    // Events received entirely behind a card/image are also consumed.
    assert(!epaper_animation_prepare(&a, 6, MUSE_MODE_BOOT, false, true, 106000));
    assert(!epaper_animation_prepare(&a, 6, MUSE_MODE_BOOT, true, true, 107000));

    assert(epaper_animation_prepare(&a, 7, MUSE_MODE_LISTENING, true, true, 108000));
    epaper_animation_presented(&a, 109000, false, 450);
    assert(a.frame == a.last && a.next_ms == 0);
    assert(!epaper_animation_prepare(&a, 7, MUSE_MODE_LISTENING, true, true, 110000));

    assert(epaper_animation_prepare(&a, 8, MUSE_MODE_BOOT, true, false, 111000));
    assert(a.last == 0 && epaper_animation_pose_time(&a) == 2.0f);
    epaper_animation_presented(&a, 112000, true, 450);
    assert(a.next_ms == 0);
}

// Render and dither exactly the character area used by the board. Optional
// output directory makes these same test frames available for visual review.
static void frames(const char *out) {
    enum { N = 112 };
    uint16_t row[N];
    uint8_t gray[N * N], bits[N * N / 8], previous[sizeof(bits)];
    int16_t err[2 * (N + 2)];
    const muse_mode_t modes[] = {MUSE_MODE_BOOT, MUSE_MODE_LISTENING};
    const char *names[] = {"boot", "listening"};
    muse_pixel_set_size(N);
    for (int m = 0; m < 2; m++) {
        epaper_animation_t a = {0};
        epaper_animation_prepare(&a, 0, modes[m], true, true, 0);
        for (int f = 0; f < 3; f++) {
            a.frame = f;
            muse_pose_t p = {.mode = modes[m], .mode_t = epaper_animation_pose_time(&a)};
            for (int i = 0; i < 10; i++) {
                p.t += 0.2f;
                muse_pixel_render(&p);
            }
            for (int y = 0; y < N; y++) {
                muse_pixel_scale(row, N, 0, N - 1, y, y);
                for (int x = 0; x < N; x++) gray[y * N + x] = row[x] ? luma565(row[x]) : 255;
            }
            dither_frame(gray, bits, N, N, err);
            assert(!f || memcmp(previous, bits, sizeof(bits)) != 0);
            memcpy(previous, bits, sizeof(bits));
            if (out) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s-%d.pgm", out, names[m], f);
                FILE *file = fopen(path, "wb");
                assert(file);
                fprintf(file, "P5\n%d %d\n255\n", N, N);
                for (int i = 0; i < N * N; i++) fputc(bits[i / 8] & (128 >> (i % 8)) ? 255 : 0, file);
                fclose(file);
            }
        }
    }
}

int main(int argc, char **argv) {
    scheduler();
    frames(argc > 1 ? argv[1] : NULL);
    puts("PASS: one-shot deadlines, interruption, hidden content, failures, still fallback, rendered frames");
    return 0;
}
