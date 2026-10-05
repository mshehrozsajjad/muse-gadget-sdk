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

// Temperature and humidity on the Waveshare ESP32-S3-ePaper-1.54: its SHTC3
// sensor on the board's I2C bus.

#pragma once

#include <stdbool.h>

bool climate_154_init(void);
// One measurement: degrees Celsius, corrected for the board's own warmth,
// and relative humidity in percent. False if the sensor didn't answer.
bool climate_154_read(float *celsius, float *humidity);
