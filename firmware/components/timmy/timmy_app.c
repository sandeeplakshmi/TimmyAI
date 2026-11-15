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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "timmy_audio.h"
#include "timmy_battery.h"
#include "timmy_board.h"
#include "timmy_ble.h"
#include "timmy_chat.h"
#include "timmy_input.h"
#include "timmy_settings.h"
#include "timmy_state.h"
#include "timmy_text.h"
#include "timmy_ui.h"
#include "timmy_voice.h"
#include "timmy_wifi.h"

static const char *TAG = "timmy";

/* Applies saved settings to hardware; runs in whichever task changed them. */
static void on_setting(timmy_setting_t what)
{
    switch (what) {
    case TIMMY_SETTING_VOLUME:
        timmy_audio_set_volume(timmy_settings_volume());
        break;
    case TIMMY_SETTING_MIC_GAIN:
        timmy_audio_set_mic_gain(timmy_settings_mic_gain());
        break;
    case TIMMY_SETTING_WIFI:
        timmy_wifi_apply();
        break;
    case TIMMY_SETTING_BLE:
        timmy_ble_apply();
        break;
    case TIMMY_SETTING_HATCH:
        timmy_hatch_config_changed();
        break;
    default:
        break;   /* brightness, sleep and the speaker are polled where they're used */
    }
}

const timmy_board_t *timmy_board;

void timmy_app_run(const timmy_board_t *board)
{
    timmy_board = board;
    ESP_LOGI(TAG, "board: %s", board->name);
    ESP_ERROR_CHECK(board->init());
    ESP_ERROR_CHECK(timmy_settings_init());
    timmy_settings_set_listener(on_setting);
    timmy_state_init();
    timmy_battery_init();
    timmy_state_set_caption("WAKING UP...");
    /* Before the UI: the touch boards' settings show Phone camera only when
     * a transport is there as they're built. Its server waits for Wi-Fi. */
    ESP_ERROR_CHECK(timmy_ui_start());
    ESP_LOGI(TAG, "UI built: free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    QueueHandle_t q = xQueueCreate(16, sizeof(timmy_input_event_t));
    ESP_ERROR_CHECK(timmy_input_start(q));

    /* Let the boot animation (flame ignites, eyes open) play out. */
    vTaskDelay(pdMS_TO_TICKS(1400));
    if (!board->audio_init && !board->mic_init) {
        ESP_LOGI(TAG, "display-only board: no microphone or speaker");
        timmy_state_set_mode(TIMMY_MODE_IDLE);
        timmy_state_set_caption("%s", "");
    } else if (timmy_voice_start(q) != ESP_OK) {
        ESP_LOGE(TAG, "voice pipeline unavailable");
    } else {
        timmy_state_set_mode(TIMMY_MODE_IDLE);
        timmy_state_set_caption("%s", "");   /* the button icons say how to talk */
    }

    timmy_hatch_start();
    /* Home Link owns the radios; these just hand it the saved settings. */
    timmy_wifi_apply();
    timmy_ble_apply();
    ESP_LOGI(TAG, "ready: free heap %u internal, %u psram",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
