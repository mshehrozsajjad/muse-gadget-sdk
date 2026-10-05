// Copyright (c) Meta Platforms, Inc. and affiliates.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "muse_pixel.h"

// Display-task-owned one-shot scheduler. Deadlines start AFTER the panel
// finishes refreshing, so slow refreshes never cause a burst of queued frames.
typedef struct {
    bool initialized;
    uint32_t generation;
    muse_mode_t mode;
    int frame, last;
    int64_t next_ms;
} epaper_animation_t;

// Connection setup changes several times during the first full refresh.
// Keep the intro through those routine changes, including a short final
// hold, but let the caller's urgent/content checks interrupt it.
static inline bool epaper_animation_keep_boot(const epaper_animation_t *a,
                                              bool enabled, bool allowed, int64_t now_ms) {
    return enabled && allowed && (!a->initialized ||
           (a->mode == MUSE_MODE_BOOT && a->last &&
            (a->frame < a->last || a->next_ms > now_ms)));
}

static inline bool epaper_animation_prepare(epaper_animation_t *a, uint32_t generation,
                                           muse_mode_t mode, bool visible, bool enabled,
                                           int64_t now_ms) {
    bool changed = !a->initialized || a->generation != generation || a->mode != mode;
    if (changed) {
        *a = (epaper_animation_t){.initialized = true, .generation = generation,
                                 .mode = mode};
        a->last = enabled && (mode == MUSE_MODE_BOOT || mode == MUSE_MODE_LISTENING) ? 2 : 0;
    }
    if (!visible) {
        // Consume the trigger: uncovering the same state must not replay it.
        a->frame = a->last;
        a->next_ms = 0;
        return false;
    }
    if (!changed && a->next_ms && now_ms >= a->next_ms) {
        a->next_ms = 0;
        if (a->frame < a->last) {
            a->frame++;
            return true;
        }
    }
    return changed;
}

static inline void epaper_animation_presented(epaper_animation_t *a, int64_t now_ms,
                                             bool success, int hold_ms) {
    if (!success) {
        a->frame = a->last;
        a->next_ms = 0;
    } else if (a->last && !a->next_ms &&
               (a->frame < a->last || a->mode == MUSE_MODE_BOOT)) {
        a->next_ms = now_ms + hold_ms;
    }
}

static inline float epaper_animation_pose_time(const epaper_animation_t *a) {
    if (!a->last) return 2.0f; // original settled still pose
    static const float boot[] = {0.15f, 1.05f, 2.0f};
    static const float listening[] = {0.0f, 0.45f, 1.0f};
    return a->mode == MUSE_MODE_BOOT ? boot[a->frame] : listening[a->frame];
}
