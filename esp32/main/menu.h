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

// The dial's menu (CONFIG_HOMEHUB_MENU): a dial press on the main screen opens
// it, turning moves the selection, a press picks. The paddle closes it, and so
// does leaving it alone for a while. What it shows is drawn by the display
// (led_status_show_menu); this keeps its state and does what's picked. All
// of it is safe from any task.

#pragma once

#include <stdbool.h>

bool menu_init(void);
bool menu_is_open(void);
void menu_open(void);
void menu_close(void);
// Dial clicks, positive clockwise. Ignored while the menu is closed.
void menu_turn(int steps);
// A dial press. Ignored while the menu is closed.
void menu_press(void);
// From the paddle's button: true if it was for the menu (a press that closed
// it, and that press's release), so it isn't push-to-talk.
bool menu_take_paddle(bool pressed);
