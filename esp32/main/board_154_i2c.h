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

// The Waveshare ESP32-S3-ePaper-1.54's I2C bus (SDA GPIO47, SCL GPIO48),
// shared by the ES8311 codec, the SHTC3 sensor and the PCF85063 clock.

#pragma once

#include "driver/i2c_master.h"

// The bus, set up on first use; NULL if it can't be.
i2c_master_bus_handle_t board_154_i2c_bus(void);
