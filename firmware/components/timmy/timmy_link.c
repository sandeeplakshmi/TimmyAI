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

#include "timmy_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include "timmy_state.h"

static const char *TAG = "timmy_link";
static const timmy_link_ops_t *s_ops;
static volatile timmy_link_state_t s_state = TIMMY_LINK_BOOT;

void timmy_link_register(const timmy_link_ops_t *ops)
{
    s_ops = ops;
}

void timmy_link_set_state(timmy_link_state_t state)
{
    if (state != s_state) {
        s_state = state;
        if (state == TIMMY_LINK_CONFIRM) {
            timmy_state_poke();      /* keep the screen awake for the prompt */
        }
    }
}

timmy_link_state_t timmy_link_state(void)
{
    return s_state;
}

const char *timmy_link_state_name(timmy_link_state_t state)
{
    switch (state) {
    case TIMMY_LINK_BOOT: return "Starting";
    case TIMMY_LINK_UNPAIRED: return "Ready to pair";
    case TIMMY_LINK_PAIRING: return "App connected";
    case TIMMY_LINK_CONFIRM: return "Confirm pairing";
    case TIMMY_LINK_CONNECTING: return "Connecting";
    case TIMMY_LINK_ONLINE: return "Online";
    case TIMMY_LINK_OFFLINE: return "Offline";
    case TIMMY_LINK_ERROR: return "Error";
    }
    return "";
}

bool timmy_link_hatch_linked(void)
{
    return s_ops && s_ops->hatch_linked && s_ops->hatch_linked();
}

const char *timmy_link_sdk_token(void)
{
    return s_ops && s_ops->sdk_token ? s_ops->sdk_token() : NULL;
}

bool timmy_link_sdk_token_set(const char *token)
{
    return s_ops && s_ops->sdk_token_set && s_ops->sdk_token_set(token);
}

bool timmy_link_sdk_token_changed(void)
{
    return s_ops && s_ops->sdk_token_changed && s_ops->sdk_token_changed();
}

bool timmy_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                        char **vm_token)
{
    return s_ops && s_ops->hatch_vm && s_ops->hatch_vm(want_vm, vm_id, id_cap, vm_name, name_cap, vm_token);
}

bool timmy_link_talk_press(void)
{
    return s_ops && s_ops->talk_press && s_ops->talk_press();
}

void timmy_link_reset_setup(void)
{
    if (s_ops && s_ops->reset_setup) {
        s_ops->reset_setup();
    }
}

bool timmy_link_wifi_get(char *ssid, char *pass)
{
    if (!s_ops || !s_ops->wifi_get) {
        return false;
    }
    s_ops->wifi_get(ssid, pass);
    return true;
}

bool timmy_link_wifi_set(const char *ssid, const char *pass)
{
    if (!s_ops || !s_ops->wifi_set) {
        return false;
    }
    s_ops->wifi_set(ssid, pass);
    return true;
}

bool timmy_link_ble_started(void)
{
    return s_ops && s_ops->ble_started && s_ops->ble_started();
}

bool timmy_link_req_ready(void)
{
    return s_ops && s_ops->req_ready && s_ops->req_ready();
}

int64_t timmy_link_req_open(const char *verb, const char *path, const char *const *headers, bool end_body,
                           timmy_link_req_cb cb, void *ctx)
{
    return s_ops && s_ops->req_open ? s_ops->req_open(verb, path, headers, end_body, cb, ctx) : 0;
}

bool timmy_link_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms)
{
    return s_ops && s_ops->req_send && s_ops->req_send(id, data, len, end_body, wait_ms);
}

void timmy_link_req_cancel(int64_t id)
{
    if (s_ops && s_ops->req_cancel) {
        s_ops->req_cancel(id);
    }
}

void timmy_link_ble_apply(void)
{
    if (s_ops && s_ops->ble_apply) {
        s_ops->ble_apply();
    }
}

/* ---- timmy_wifi.h, backed by Link's wifi_mgr ---- */

esp_err_t timmy_wifi_start(void)
{
    timmy_wifi_apply();
    return ESP_OK;
}

void timmy_wifi_apply(void)
{
    if (s_ops && s_ops->wifi_apply) {
        s_ops->wifi_apply();
    }
}

void timmy_wifi_status(timmy_wifi_status_t *out)
{
    if (s_ops && s_ops->wifi_status) {
        s_ops->wifi_status(out);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = TIMMY_WIFI_OFF;
    strlcpy(out->detail, "Starting...", sizeof(out->detail));
}

bool timmy_wifi_connected(void)
{
    timmy_wifi_status_t w;
    timmy_wifi_status(&w);
    return w.state == TIMMY_WIFI_CONNECTED;
}

static timmy_wifi_power_t s_power_level = TIMMY_WIFI_FULL;
static wifi_ps_type_t s_power_before;   /* none, or modem sleep for BLE coexistence */
static int s_awake_holds;               /* timmy_wifi_keep_awake */
static portMUX_TYPE s_power_lock = portMUX_INITIALIZER_UNLOCKED;

/* What the level (and any hold) asks of the radio. */
static wifi_ps_type_t power_ps(void)
{
    portENTER_CRITICAL(&s_power_lock);
    timmy_wifi_power_t level = s_power_level;
    bool held = s_awake_holds > 0;
    portEXIT_CRITICAL(&s_power_lock);
    /* Resting, the radio wakes for every tenth beacon (the listen interval
     * wifi_mgr joins with, about 1 s), not every DTIM; replies can wait that
     * long. */
    return held                      ? WIFI_PS_NONE
           : level == TIMMY_WIFI_REST ? WIFI_PS_MAX_MODEM
           : level == TIMMY_WIFI_DOZE ? WIFI_PS_MIN_MODEM
                                     : s_power_before;
}

void timmy_wifi_power(timmy_wifi_power_t level)
{
    if (level == s_power_level) {
        return;
    }
    if (s_ops && s_ops->power_save) {
        s_ops->power_save(level == TIMMY_WIFI_REST);
    }
    if (s_power_level == TIMMY_WIFI_FULL && s_awake_holds == 0 && esp_wifi_get_ps(&s_power_before) != ESP_OK) {
        s_power_before = WIFI_PS_NONE;   /* as wifi_mgr starts it */
    }
    portENTER_CRITICAL(&s_power_lock);
    s_power_level = level;
    portEXIT_CRITICAL(&s_power_lock);
    esp_wifi_set_ps(power_ps());
}

void timmy_link_ble_quiet(bool quiet)
{
    static int holds;
    portENTER_CRITICAL(&s_power_lock);
    int was = holds;
    holds += quiet ? 1 : (holds > 0 ? -1 : 0);
    int now = holds;
    portEXIT_CRITICAL(&s_power_lock);
    if ((was == 0) != (now == 0) && s_ops && s_ops->ble_quiet) {
        s_ops->ble_quiet(now > 0);
    }
}

void timmy_wifi_keep_awake(bool on)
{
    wifi_ps_type_t was = WIFI_PS_NONE;
    esp_wifi_get_ps(&was);
    portENTER_CRITICAL(&s_power_lock);
    bool first = on && s_awake_holds == 0;
    s_awake_holds += on ? 1 : (s_awake_holds > 0 ? -1 : 0);
    portEXIT_CRITICAL(&s_power_lock);
    if (first && s_power_level == TIMMY_WIFI_FULL) {
        s_power_before = was;   /* what FULL means once the hold goes */
    }
    esp_err_t err = esp_wifi_set_ps(power_ps());
    ESP_LOGI(TAG, "wifi power save %d -> %d%s (%s)", (int)was, (int)power_ps(), on ? ", held awake" : "",
             esp_err_to_name(err));
}

void timmy_wifi_nap(bool nap)
{
    if (s_ops && s_ops->wifi_nap) {
        s_ops->wifi_nap(nap);
    }
}

esp_err_t timmy_wifi_scan(void)
{
    return s_ops && s_ops->wifi_scan ? s_ops->wifi_scan() : ESP_ERR_INVALID_STATE;
}

bool timmy_wifi_scanning(void)
{
    return s_ops && s_ops->wifi_scanning && s_ops->wifi_scanning();
}

int timmy_wifi_scan_results(timmy_wifi_ap_t *out, int max, uint32_t *gen)
{
    if (s_ops && s_ops->wifi_scan_results) {
        return s_ops->wifi_scan_results(out, max, gen);
    }
    *gen = 0;
    return 0;
}

int timmy_wifi_saved(timmy_wifi_saved_t *out, int max)
{
    return s_ops && s_ops->wifi_saved ? s_ops->wifi_saved(out, max) : 0;
}

void timmy_wifi_forget(const char *ssid)
{
    if (s_ops && s_ops->wifi_forget) {
        s_ops->wifi_forget(ssid);
    }
}
