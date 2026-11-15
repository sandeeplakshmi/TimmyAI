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

#include "led_status.h"
#include "stack_monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "timmy_glue.h"
#include "timmy_ui.h"

static const char *TAG = "link.led";

// Timmy Home Link RGB LED: 3 channels, driven via LEDC PWM.


static SemaphoreHandle_t s_mutex = NULL;
static led_state_t s_state = LED_STATE_BOOT;
static TaskHandle_t s_task = NULL;
// Title requested by led_status_set_title(); the LED task draws it.
static char s_title[48];
static bool s_title_dirty = false;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t COLOR_OFF      = {0,   0,   0};
static const rgb_t COLOR_DIM_ORANGE = {12, 4,   0};
static const rgb_t COLOR_ORANGE   = {80,  25,  0};
static const rgb_t COLOR_BLUE     = {0,   0,   80};
static const rgb_t COLOR_YELLOW   = {80,  50,  0};
static const rgb_t COLOR_GREEN    = {0,   80,  0};
static const rgb_t COLOR_RED      = {80,  0,   0};
static const rgb_t COLOR_PURPLE   = {60,  0,   60};

// Boards with the full UI show Link's status on screen instead of an LED.
static void led_hw_set_color(rgb_t c) {
    (void)c;
}

static bool led_hw_init(void) {
    ESP_LOGI(TAG, "LED status ready: Timmy display");
    return true;
}

static void led_hw_set_connected(void) {
    led_hw_set_color(COLOR_GREEN);
}

static void led_hw_set_title(const char *text) {
    (void)text;
}

static void led_hw_set_dimmed(rgb_t c, float level) {
    led_hw_set_color((rgb_t){c.r * level, c.g * level, c.b * level});
}

static void blink(rgb_t c, int on_ms, int off_ms) {
    led_hw_set_color(c);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    led_hw_set_color(COLOR_OFF);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void breathe(rgb_t c, int period_ms) {
    int steps = 30;
    int step_ms = period_ms / (steps * 2);
    for (int i = 0; i < steps; i++) {
        led_hw_set_dimmed(c, (float)i / steps);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    for (int i = steps; i > 0; i--) {
        led_hw_set_dimmed(c, (float)i / steps);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

// Wait between steady frames. A voice state change cuts the wait short.
static void led_wait(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}


static void led_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    while (1) {
        stack_monitor_poll(&stack);
        char title[sizeof(s_title)];
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        led_state_t state = s_state;
        bool title_dirty = s_title_dirty;
        if (title_dirty) memcpy(title, s_title, sizeof(title));
        s_title_dirty = false;
        xSemaphoreGive(s_mutex);
        if (title_dirty) led_hw_set_title(title);

        switch (state) {
            case LED_STATE_BOOT:
                // Orange at boot — waiting for connectivity.
                led_hw_set_color(COLOR_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_SETUP_IDLE:
                led_hw_set_color(COLOR_DIM_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_BLE_ADVERTISING:
                breathe(COLOR_ORANGE, 2000);
                break;
            case LED_STATE_BLE_CONNECTED:
                led_hw_set_color(COLOR_ORANGE);
                led_wait(200);
                break;
            case LED_STATE_PAIRING_CONFIRM_REQUIRED:
                breathe(COLOR_BLUE, 2000);
                break;
            case LED_STATE_WIFI_CONNECTING:
            case LED_STATE_WIFI_CONNECTED:
                // Solid blue — confirmed and working through Wi-Fi/auth/VM
                // setup. confirm_required pulses; green means the tunnel is up.
                led_hw_set_color(COLOR_BLUE);
                led_wait(200);
                break;
            case LED_STATE_AUTH_OK:
            case LED_STATE_VM_OK:
                // Solid blue — WiFi/auth is accepted, VM/control path is coming up.
                led_hw_set_color(COLOR_BLUE);
                led_wait(200);
                break;
            case LED_STATE_WS_CONNECTED:
                // Green — tunnel is live (a green dot on the display).
                led_hw_set_connected();
                led_wait(200);
                break;
            case LED_STATE_VM_SWITCHING:
                blink(COLOR_YELLOW, 150, 150);
                break;
            case LED_STATE_WS_DISCONNECTED:
                blink(COLOR_YELLOW, 500, 500);
                break;
            case LED_STATE_UNPAIRED:
                led_hw_set_color(COLOR_PURPLE);
                led_wait(200);
                break;
            case LED_STATE_ERROR:
                blink(COLOR_RED, 200, 200);
                break;
        }
    }
}

bool led_status_init(void) {
    if (!led_hw_init()) {
        return false;
    }

    return true;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    led_hw_set_color(COLOR_ORANGE);
    // 2026-09-20: increase margin; measured only 904 bytes free with a 2048-byte stack.
    const uint32_t stack = 3072;
    xTaskCreate(led_task, "led", stack, NULL, 2, &s_task);
    return true;
}

void led_status_set_state(led_state_t state) {
    timmy_glue_led_state(state);
    return;
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_mutex);
}

void led_status_set_title(const char *title) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    s_title_dirty = true;
    xSemaphoreGive(s_mutex);
}


bool led_status_display_info(int *width, int *height) {
#if   CONFIG_HOMEHUB_LED_BACKEND_TIMMY && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    return timmy_ui_image_size(width, height);
#else
    (void)width;
    (void)height;
    return false;
#endif
}

int led_status_display_bits(void) {
#if   CONFIG_HOMEHUB_LED_BACKEND_TIMMY && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    int w, h;
    return timmy_ui_image_size(&w, &h) ? timmy_ui_image_bits() : 0;
#else
    return 0;
#endif
}

void led_status_draw_done(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_TIMMY && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    timmy_ui_image_done();
#endif
}


bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
#if   CONFIG_HOMEHUB_LED_BACKEND_TIMMY && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    return timmy_ui_image_draw(x, y, w, h, pixels);
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)pixels;
    return false;
#endif
}

void led_status_show_animation(void) {
#if   CONFIG_HOMEHUB_LED_BACKEND_TIMMY && CONFIG_HOMEHUB_DISPLAY_COMMANDS
    timmy_ui_image_hide();
#endif
}
