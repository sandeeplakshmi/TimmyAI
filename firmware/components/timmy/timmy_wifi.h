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
#include <stdint.h>

#include "esp_err.h"

#include "timmy_settings.h"

/*
 * Wi-Fi station driven by timmy_settings: joins whichever saved network is in
 * range, reconnects with backoff, and runs scans for the settings screen. All
 * calls are safe from any task; status is copied out for the UI to poll.
 */

typedef enum {
    TIMMY_WIFI_OFF,
    TIMMY_WIFI_NO_NETWORK,     /* on, but nothing saved */
    TIMMY_WIFI_CONNECTING,
    TIMMY_WIFI_CONNECTED,
    TIMMY_WIFI_FAILED,         /* gave up (e.g. wrong password) */
    TIMMY_WIFI_NOT_NEARBY,     /* none of the saved networks showed up in a scan; looking again less often */
} timmy_wifi_state_t;

typedef struct {
    timmy_wifi_state_t state;
    char ssid[TIMMY_SSID_MAX + 1];
    char ip[16];
    int rssi;
    char detail[40];          /* human-readable reason while connecting/failed */
} timmy_wifi_status_t;

typedef struct {
    char ssid[TIMMY_SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} timmy_wifi_ap_t;

#define TIMMY_WIFI_SAVED_MAX 8
typedef struct {
    char ssid[TIMMY_SSID_MAX + 1];
    bool hidden;              /* joined by name; doesn't show in scans */
} timmy_wifi_saved_t;

esp_err_t timmy_wifi_start(void);
/* Re-reads on/off and credentials from settings and (re)connects. */
void timmy_wifi_apply(void);
void timmy_wifi_status(timmy_wifi_status_t *out);
bool timmy_wifi_connected(void);
typedef enum {
    TIMMY_WIFI_FULL,   /* the mode from before: no modem sleep, but for BLE coexistence */
    TIMMY_WIFI_DOZE,   /* modem sleep between DTIM beacons: a few hundred ms extra latency */
    TIMMY_WIFI_REST,   /* over several beacons, and Link's session polls less: up to a second */
} timmy_wifi_power_t;
void timmy_wifi_power(timmy_wifi_power_t level);
/* Holds Wi-Fi out of modem sleep whatever the level (a video stream: the
 * access point would otherwise hold each packet for the next beacon, up to
 * 100 ms). Calls nest; false releases one. */
void timmy_wifi_keep_awake(bool on);
/* Leaves Wi-Fi, dropping Link's session and Hatch's, until called with false,
 * which rejoins at once. */
void timmy_wifi_nap(bool nap);

esp_err_t timmy_wifi_scan(void);
bool timmy_wifi_scanning(void);
/* Copies the latest results (strongest first); *gen changes when they do. */
int timmy_wifi_scan_results(timmy_wifi_ap_t *out, int max, uint32_t *gen);

/* Saved networks, most recently joined first. Returns the count. */
int timmy_wifi_saved(timmy_wifi_saved_t *out, int max);
/* Forgets one saved network, disconnecting first if it's the one in use. */
void timmy_wifi_forget(const char *ssid);
