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

// A rotary dial with a push switch (CONFIG_HOMEHUB_DIAL), an EC11 or alike:
// A, B and the push each pull their GPIO to ground.

#pragma once

#include <stdbool.h>

typedef void (*dial_press_cb)(void);
// `steps`: clicks turned since the last call, positive forward.
typedef void (*dial_turn_cb)(int steps);

// Starts watching the dial. The callbacks run on the dial's task: a press
// once it's released, turns as they come. Either also wakes the chip from
// light sleep.
bool dial_init(dial_press_cb on_press, dial_turn_cb on_turn);
