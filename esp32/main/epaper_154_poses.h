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

// The character for the 1.54 inch e-paper, drawn in black and white at the
// screen's own 112 x 112 character size: one pose per state, and three entry
// keyframes for boot and listening.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define EPAPER_POSE_SIZE   112
#define EPAPER_POSE_FRAMES 3   // keyframes 0 to 2; 2 is the settled pose

typedef enum {
    EPAPER_POSE_BOOT,
    EPAPER_POSE_IDLE,
    EPAPER_POSE_LISTENING,
    EPAPER_POSE_THINKING,
    EPAPER_POSE_SPEAKING,
    EPAPER_POSE_ERROR,
    EPAPER_POSE_SLEEP,
} epaper_pose_t;

// Sets up the scratch images. False without memory.
bool epaper_154_poses_init(void);

// Draws `pose` at keyframe `frame` into `out`, EPAPER_POSE_SIZE squared bytes,
// row by row: 0 for black, 255 for white.
void epaper_154_pose_render(uint8_t *out, epaper_pose_t pose, int frame);
