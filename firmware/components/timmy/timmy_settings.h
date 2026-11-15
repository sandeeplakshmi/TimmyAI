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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used,
 * as is the reply text size, which notifies nothing.
 */

#define TIMMY_SSID_MAX 32
#define TIMMY_PASS_MAX 64
#define TIMMY_HOST_MAX 63
#define TIMMY_VM_MAX 63
#define TIMMY_TOKEN_MAX 1023

#define TIMMY_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */

typedef enum {
    TIMMY_SETTING_VOLUME,
    TIMMY_SETTING_SPEAKER,
    TIMMY_SETTING_MIC_GAIN,
    TIMMY_SETTING_BRIGHTNESS,
    TIMMY_SETTING_SLEEP,
    TIMMY_SETTING_WIFI,          /* on/off or credentials */
    TIMMY_SETTING_BLE,
    TIMMY_SETTING_HATCH,
    TIMMY_SETTING_VOICE_NOTES,
} timmy_setting_t;

typedef void (*timmy_setting_cb_t)(timmy_setting_t what);

esp_err_t timmy_settings_init(void);
void timmy_settings_set_listener(timmy_setting_cb_t cb);

int timmy_settings_volume(void);         /* 0..100 */
bool timmy_settings_speaker_on(void);    /* off: replies are shown, not played */
int timmy_settings_mic_gain(void);       /* dB, 0..TIMMY_MIC_GAIN_MAX */
int timmy_settings_brightness(void);     /* 10..100 */
int timmy_settings_sleep_s(void);        /* 0 = never */
/* Reply text size on small screens with full-screen replies. */
enum { TIMMY_TEXT_SMALL, TIMMY_TEXT_MEDIUM, TIMMY_TEXT_LARGE };
int timmy_settings_text_size(void);
/* Which face the home screen shows: an index into timmy_avatar.h's list (0: Timmy's eyes). */
int timmy_settings_avatar(void);
void timmy_settings_set_avatar(int avatar);
bool timmy_settings_wifi_on(void);
bool timmy_settings_ble_on(void);
/* StopWatch: false selects realtime PTT; true selects voice notes only. */
bool timmy_settings_voice_notes(void);
void timmy_settings_set_voice_notes(bool on);

void timmy_settings_wifi(char ssid[TIMMY_SSID_MAX + 1], char pass[TIMMY_PASS_MAX + 1]);
void timmy_settings_hatch_host(char out[TIMMY_HOST_MAX + 1]);
void timmy_settings_hatch_vm(char out[TIMMY_VM_MAX + 1]);
void timmy_settings_hatch_token(char out[TIMMY_TOKEN_MAX + 1]);
size_t timmy_settings_hatch_token_len(void);

void timmy_settings_set_volume(int pct);
void timmy_settings_set_speaker_on(bool on);
void timmy_settings_set_mic_gain(int db);
void timmy_settings_set_brightness(int pct);
void timmy_settings_set_sleep_s(int secs);
void timmy_settings_set_text_size(int size);
void timmy_settings_set_wifi_on(bool on);
void timmy_settings_set_ble_on(bool on);
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void timmy_settings_set_wifi(const char *ssid, const char *pass);
void timmy_settings_set_hatch_host(const char *host);
void timmy_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t timmy_settings_set_hatch_token(const char *token, bool append);
