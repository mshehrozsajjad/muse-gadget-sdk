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

// Pin, attenuation and divider: Waveshare's V2 ADC example
// (github.com/waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2/01_ADC_Test/components/adc_bsp/adc_bsp.c).

#include "battery_154.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "link.battery";

#define BATTERY_ADC_UNIT    ADC_UNIT_1
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3  // GPIO4
#define BATTERY_DIVIDER     2
#define BATTERY_SAMPLES     8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

bool battery_154_init(void) {
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = BATTERY_ADC_UNIT};
    const adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err == ESP_OK) err = adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan);
    if (err == ESP_OK) err = adc_cali_create_scheme_curve_fitting(&cali, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

int battery_154_millivolts(void) {
    if (!s_cali) return 0;
    int sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) != ESP_OK
            || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            return 0;
        }
        sum += mv;
    }
    return sum / BATTERY_SAMPLES * BATTERY_DIVIDER;
}

int battery_154_percent(int millivolts) {
    // A typical single-cell LiPo discharge curve at light load.
    static const struct {
        int mv, pct;
    } curve[] = {
        {3300, 0},  {3500, 5},  {3600, 12}, {3650, 20}, {3700, 30}, {3750, 40},
        {3800, 50}, {3900, 65}, {4000, 78}, {4100, 90}, {4200, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (millivolts <= curve[0].mv) return 0;
    if (millivolts >= curve[n - 1].mv) return 100;
    for (int i = 1; i < n; i++) {
        if (millivolts < curve[i].mv) {
            int span_mv = curve[i].mv - curve[i - 1].mv;
            int span_pct = curve[i].pct - curve[i - 1].pct;
            return curve[i - 1].pct + (millivolts - curve[i - 1].mv) * span_pct / span_mv;
        }
    }
    return 100;
}
