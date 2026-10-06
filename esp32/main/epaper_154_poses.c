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

// Yolk's pose study, revision 4 (design/bot-animations/pose-study-v4/poses.c in
// the Yolk repository), as firmware: the same integer polygons, lines, disks
// and ordered shading, drawn straight into one bit, so the panel shows the
// approved pixels with no grayscale or dithering in between. Keep the
// geometry identical to the study; its export.py renders the reference.

#include "epaper_154_poses.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

enum { W = EPAPER_POSE_SIZE, H = EPAPER_POSE_SIZE };

typedef struct {
    int x, y;
} point;

static uint8_t *s_canvas;    // the image being drawn: the caller's
static uint8_t *s_backdrop;  // paper flecks, kept apart from the character
static uint8_t *s_actor;     // the character before its whole-body transform
static epaper_pose_t mode;
static int frame;

static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static void pixel(int x, int y, int c) {
    if (x >= 0 && x < W && y >= 0 && y < H) s_canvas[y * W + x] = (uint8_t)c;
}

static void disk(int x, int y, int radius, int c) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) pixel(x + dx, y + dy, c);
        }
    }
}

// Bresenham, with a disk of `radius` at each step.
static void line(point a, point b, int radius, int c) {
    int dx = abs(b.x - a.x), sx = a.x < b.x ? 1 : -1;
    int dy = -abs(b.y - a.y), sy = a.y < b.y ? 1 : -1, e = dx + dy;
    for (;;) {
        disk(a.x, a.y, radius, c);
        if (a.x == b.x && a.y == b.y) break;
        int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            a.x += sx;
        }
        if (e2 <= dx) {
            e += dx;
            a.y += sy;
        }
    }
}

// Even-odd point-in-polygon.
static int inside(int x, int y, const point *p, int n) {
    int hit = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((p[i].y > y) != (p[j].y > y)) {
            int at = p[i].x + (y - p[i].y) * (p[j].x - p[i].x) / (p[j].y - p[i].y);
            if (x < at) hit = !hit;
        }
    }
    return hit;
}

static void shape(const point *p, int n, int fill, int stroke) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (inside(x, y, p, n)) pixel(x, y, fill);
        }
    }
    for (int i = 0; i < n; i++) line(p[i], p[(i + 1) % n], stroke, 0);
}

#define SHAPE(p, fill, stroke) shape(p, (int)(sizeof(p) / sizeof(*(p))), fill, stroke)

static void sparkle(int x, int y, int r) {
    point p[] = {{x, y - r}, {x + 1, y - 1}, {x + r, y}, {x + 1, y + 1},
                 {x, y + r}, {x - 1, y + 1}, {x - r, y}, {x - 1, y - 1}};
    SHAPE(p, 255, 0);
}

// A few discrete paper flecks, not a halo or glow turned to noise.
static void draw_backdrop(void) {
    sparkle(15, 33, 4);
    if (mode == EPAPER_POSE_IDLE || mode == EPAPER_POSE_BOOT) sparkle(94, 27, 4);
    sparkle(24, 18, 2);
    pixel(85, 18, 0);
    pixel(86, 18, 0);
    pixel(85, 19, 0);
    pixel(86, 19, 0);
    const point fleck[] = {{16, 48}, {18, 48}, {18, 50}, {16, 50}};
    SHAPE(fleck, 255, 0);
    const point fleck2[] = {{94, 44}, {96, 44}, {96, 46}, {94, 46}};
    if (mode == EPAPER_POSE_IDLE || mode == EPAPER_POSE_BOOT) SHAPE(fleck2, 255, 0);
}

// The tall hood with straighter sides and a gently rounded base, its feet,
// and paper-fold shading on the base and right edge only.
static void draw_body(void) {
    const point foot_l[] = {{39, 94}, {48, 94}, {48, 99}, {46, 101}, {39, 101}, {37, 99}};
    const point foot_r[] = {{64, 94}, {73, 94}, {75, 99}, {72, 101}, {66, 101}, {64, 99}};
    SHAPE(foot_l, 255, 1);
    SHAPE(foot_r, 255, 1);
    const point body[] = {{48, 12}, {63, 12}, {70, 15}, {76, 22}, {80, 32}, {82, 45},
                          {82, 81}, {80, 89}, {75, 95}, {68, 98}, {44, 98}, {37, 95},
                          {32, 89}, {30, 81}, {30, 45}, {32, 32}, {36, 22}, {42, 15}};
    SHAPE(body, 255, 1);
    // A deterministic 4x4 density pattern, drawn in one bit directly.
    for (int y = 14; y < 97; y++) {
        for (int x = 31; x < 82; x++) {
            if (!inside(x, y, body, (int)(sizeof(body) / sizeof(*body)))) continue;
            int density = 0;
            int lower = 84 + (x - 54) * (x - 54) / 180;
            if (y > lower) density = 2;
            if (x > 76 && y > 37 && y < 89) density = 1;
            if (x > 75 && y > 86) density = 5;
            if (y > 94 && x > 56) density = 6;
            if (density && bayer[y & 3][x & 3] < density) pixel(x, y, 0);
        }
    }
}

// A clean white face with each state's brows, eyes and mouth.
static void draw_face(void) {
    const point face[] = {{43, 29}, {68, 29}, {73, 32}, {76, 38}, {76, 49}, {73, 55},
                          {68, 58}, {44, 58}, {39, 55}, {36, 49}, {36, 38}, {39, 32}};
    SHAPE(face, 255, 0);

    if (mode == EPAPER_POSE_ERROR) {
        line((point){41, 36}, (point){48, 33}, 0, 0);
        line((point){64, 33}, (point){71, 36}, 0, 0);
    } else if (mode == EPAPER_POSE_THINKING) {
        line((point){41, 35}, (point){47, 36}, 0, 0);
        line((point){64, 34}, (point){67, 32}, 0, 0);
        line((point){67, 32}, (point){71, 34}, 0, 0);
    } else {
        line((point){41, 36}, (point){44, 34}, 0, 0);
        line((point){44, 34}, (point){47, 34}, 0, 0);
        line((point){65, 34}, (point){68, 34}, 0, 0);
        line((point){68, 34}, (point){71, 36}, 0, 0);
    }

    for (int e = 0; e < 2; e++) {
        int x = e ? 67 : 45;
        if (mode == EPAPER_POSE_BOOT && frame == 0) {  // asleep: closed eyes
            line((point){x - 4, 43}, (point){x - 1, 45}, 1, 0);
            line((point){x - 1, 45}, (point){x + 3, 43}, 1, 0);
        } else {
            int gaze = mode == EPAPER_POSE_THINKING ? 2 : 0;
            int y = mode == EPAPER_POSE_THINKING ? 41 : 43;
            disk(x + gaze, y, (mode == EPAPER_POSE_LISTENING && frame == 2) ? 5 : 4, 0);
            disk(x + gaze - 1, y - 2, 1, 255);
        }
    }

    if (mode == EPAPER_POSE_SPEAKING) {
        const point mouth[] = {{51, 50}, {61, 50}, {60, 54}, {58, 56}, {54, 56}, {52, 54}};
        SHAPE(mouth, 0, 0);
        line((point){55, 54}, (point){58, 54}, 0, 255);
    } else if (mode == EPAPER_POSE_ERROR) {
        line((point){52, 54}, (point){56, 51}, 1, 0);
        line((point){56, 51}, (point){60, 54}, 1, 0);
    } else if (mode == EPAPER_POSE_THINKING) {
        line((point){53, 53}, (point){56, 51}, 0, 0);
        line((point){56, 51}, (point){59, 53}, 0, 0);
    } else if ((mode == EPAPER_POSE_LISTENING && frame > 0) || (mode == EPAPER_POSE_BOOT && frame < 2)) {
        disk(56, 52, 2, 0);
    } else {
        line((point){52, 51}, (point){54, 53}, 1, 0);
        line((point){54, 53}, (point){58, 53}, 1, 0);
        line((point){58, 53}, (point){60, 51}, 1, 0);
    }
}

// Short rounded paws: each state's gesture, no long human-like forearms.
static void draw_arms(void) {
    const point arm_l[] = {{30, 59}, {26, 62}, {25, 68}, {25, 75}, {28, 78}, {32, 77}, {34, 73}, {34, 65}};
    const point arm_r[] = {{82, 59}, {86, 62}, {87, 68}, {87, 75}, {84, 78}, {80, 77}, {78, 73}, {78, 65}};
    const point worry_l[] = {{32, 68}, {35, 61}, {41, 58}, {46, 59}, {48, 64},
                             {45, 68}, {41, 70}, {39, 77}, {35, 77}};
    const point stretch_l[] = {{31, 61}, {25, 55}, {18, 44}, {18, 38}, {22, 36}, {25, 39}, {28, 47}, {35, 50}};
    if (mode == EPAPER_POSE_ERROR) {
        SHAPE(worry_l, 255, 1);
    } else if (mode == EPAPER_POSE_BOOT && frame == 1) {
        SHAPE(stretch_l, 255, 1);
    } else {
        SHAPE(arm_l, 255, 1);
    }

    const point wave[] = {{81, 61}, {88, 58}, {94, 48}, {94, 40}, {91, 37}, {88, 38}, {87, 47}, {81, 50}};
    const point ear[] = {{81, 62}, {88, 58}, {90, 52}, {90, 43}, {87, 40}, {83, 41}, {82, 47}, {78, 49}};
    // One closed oval mitten under the chin: no digit that could read as a
    // gesture of its own.
    const point chin[] = {{71, 58}, {77, 58}, {81, 61}, {82, 66}, {80, 70},
                          {75, 72}, {70, 71}, {66, 68}, {65, 64}, {67, 60}};
    const point gesture[] = {{81, 64}, {87, 62}, {92, 58}, {94, 53}, {93, 49},
                             {90, 47}, {86, 48}, {84, 52}, {79, 54}};
    const point worry_r[] = {{80, 68}, {77, 61}, {71, 58}, {66, 59}, {64, 64},
                             {67, 68}, {71, 70}, {73, 77}, {77, 77}};
    const point stretch_r[] = {{81, 61}, {87, 55}, {94, 44}, {94, 38}, {90, 36}, {87, 39}, {84, 47}, {77, 50}};
    if (mode == EPAPER_POSE_BOOT && frame == 2) {
        SHAPE(wave, 255, 1);
    } else if (mode == EPAPER_POSE_BOOT && frame == 1) {
        SHAPE(stretch_r, 255, 1);
    } else if (mode == EPAPER_POSE_ERROR) {
        SHAPE(worry_r, 255, 1);
    } else if (mode == EPAPER_POSE_LISTENING && frame > 0) {
        point moving[sizeof(ear) / sizeof(*ear)];
        for (unsigned i = 0; i < sizeof(ear) / sizeof(*ear); i++) {
            moving[i] = ear[i];
            moving[i].y += frame == 1 ? 8 : 0;
        }
        SHAPE(moving, 255, 1);
    } else if (mode == EPAPER_POSE_THINKING) {
        SHAPE(chin, 255, 1);
    } else if (mode == EPAPER_POSE_SPEAKING) {
        SHAPE(gesture, 255, 1);
    } else {
        SHAPE(arm_r, 255, 1);
    }

    if (mode == EPAPER_POSE_IDLE || mode == EPAPER_POSE_LISTENING || mode == EPAPER_POSE_THINKING
        || mode == EPAPER_POSE_SPEAKING || (mode == EPAPER_POSE_BOOT && frame != 1)) {
        line((point){33, 65}, (point){33, 72}, 0, 0);
    }
    if (mode == EPAPER_POSE_IDLE || (mode == EPAPER_POSE_BOOT && frame == 0)
        || (mode == EPAPER_POSE_LISTENING && frame == 0)) {
        line((point){79, 65}, (point){79, 72}, 0, 0);
    }
}

// Whole-body acting: squash, lean and tilt, inverse-mapping the drawn
// character onto the backdrop so holes and outlines stay clean. Idle is the
// identity.
static void act(void) {
    int height = 100, lean = 0, tilt = 0, lift = 0;
    if (mode == EPAPER_POSE_BOOT) {
        height = frame == 0 ? 58 : frame == 1 ? 96 : 100;
        lift = frame == 1 ? 6 : 0;
        lean = frame == 2 ? -3 : 0;
    } else if (mode == EPAPER_POSE_LISTENING) {
        lean = frame * 4;
        tilt = frame * 2;
    } else if (mode == EPAPER_POSE_THINKING) {
        lean = -2;
        tilt = -2;
    } else if (mode == EPAPER_POSE_SPEAKING) {
        lean = -2;
        tilt = 1;
    } else if (mode == EPAPER_POSE_ERROR) {
        height = 82;
        lean = 2;
        tilt = 2;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int sy = 101 + (y + lift - 101) * 100 / height;
            int sx = x - lean * (101 - sy) / 89;
            sy = 101 + (y + lift - 101 - tilt * (sx - 56) / 26) * 100 / height;
            sx = x - lean * (101 - sy) / 89;
            if (sx >= 0 && sx < W && sy >= 0 && sy < H && s_actor[sy * W + sx] == 0) pixel(x, y, 0);
        }
    }
}

// Small marks outside the face and silhouette: the ground, sound arcs, a
// thought, speech lines, an exclamation.
static void draw_marks(void) {
    for (int x = 30; x < 84; x++) {
        if (x % 4 == 0) pixel(x, 104, 0);
    }
    if (mode == EPAPER_POSE_LISTENING && frame > 0) {
        line((point){94, 30}, (point){97, 34}, 0, 0);
        line((point){97, 34}, (point){97, 39}, 0, 0);
        line((point){97, 39}, (point){94, 43}, 0, 0);
        if (frame == 2) {
            line((point){100, 27}, (point){104, 33}, 0, 0);
            line((point){104, 33}, (point){104, 40}, 0, 0);
            line((point){104, 40}, (point){100, 46}, 0, 0);
        }
    } else if (mode == EPAPER_POSE_THINKING) {
        disk(86, 32, 1, 0);
        disk(92, 24, 2, 0);
        disk(99, 15, 4, 0);
        disk(99, 15, 2, 255);
    } else if (mode == EPAPER_POSE_SPEAKING) {
        line((point){92, 34}, (point){98, 29}, 1, 0);
        line((point){96, 41}, (point){103, 39}, 1, 0);
    } else if (mode == EPAPER_POSE_ERROR) {
        const point mark[] = {{93, 26}, {97, 26}, {97, 35}, {95, 38}, {93, 35}};
        SHAPE(mark, 255, 0);
        disk(95, 43, 1, 0);
    }
}

bool epaper_154_poses_init(void) {
    if (s_backdrop) return true;
#ifdef ESP_PLATFORM
    s_backdrop = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
    s_actor = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
#else
    s_backdrop = malloc(W * H);
    s_actor = malloc(W * H);
#endif
    if (!s_backdrop || !s_actor) {
        free(s_backdrop);
        free(s_actor);
        s_backdrop = s_actor = NULL;
        return false;
    }
    return true;
}

void epaper_154_pose_render(uint8_t *out, epaper_pose_t pose, int at_frame) {
    if (!s_backdrop) {
        memset(out, 255, W * H);
        return;
    }
    mode = pose;
    frame = at_frame < 0 ? 0 : at_frame >= EPAPER_POSE_FRAMES ? EPAPER_POSE_FRAMES - 1 : at_frame;
    s_canvas = out;

    memset(s_canvas, 255, W * H);
    draw_backdrop();
    memcpy(s_backdrop, s_canvas, W * H);

    memset(s_canvas, 255, W * H);
    draw_body();
    draw_face();
    draw_arms();

    memcpy(s_actor, s_canvas, W * H);
    memcpy(s_canvas, s_backdrop, W * H);
    act();
    draw_marks();
}
