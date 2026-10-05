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

// Battery voltage on the Waveshare ESP32-S3-ePaper-1.54: BAT_ADC on GPIO4
// (ADC1 channel 3) through a 1:2 divider.

#pragma once

#include <stdbool.h>

bool battery_154_init(void);
// The battery voltage in millivolts, averaged over a few samples; 0 if it
// can't be read.
int battery_154_millivolts(void);
// A single-cell LiPo's charge, 0 to 100, from its resting voltage.
int battery_154_percent(int millivolts);
