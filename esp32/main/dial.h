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

// A rotary dial's push switch (CONFIG_HOMEHUB_DIAL): pressed, it pulls its
// GPIO to ground. The rotation isn't read yet.

#pragma once

#include <stdbool.h>

typedef void (*dial_cb)(void);

// Starts watching the push: `on_press` runs on the dial's task once a press
// is released. The press also wakes the chip from light sleep.
bool dial_init(dial_cb on_press);
