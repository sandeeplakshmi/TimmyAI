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
 *
 * Modified by the Timmy contributors, 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
#include "esp_psram.h"
#endif
#include "diagnostic_log.h"
#include "link_events.h"
#include "timmy_glue.h"

void app_main(void) {
#if CONFIG_HOMEHUB_SUPPORT_BUG_REPORT
    if (!diagnostic_log_init()) {
        ESP_LOGW("link.main",
                 "support log capture disabled: PSRAM unavailable");
    }
#endif
    if (!link_events_init()) {
        ESP_LOGW("link.main", "connectivity history disabled: PSRAM unavailable");
    }
    ESP_LOGI("link.main", CONFIG_GADGET_PRODUCT_NAME " starting");
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP && CONFIG_SPIRAM_IGNORE_NOTFOUND
    // The C5's TCP windows are sized for lwIP buffers in PSRAM. Its defaults
    // (sdkconfig.defaults.esp32c5) require PSRAM at boot; a config that lets
    // it boot without falls back to internal RAM.
    if (!esp_psram_is_initialized()) {
        ESP_LOGE("link.main", "PSRAM did not start: network buffers fall back to internal RAM");
    }
#endif
    timmy_glue_start();
    app_run();
}
