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

/*
 * Shared state between the voice pipeline (writer) and the UI (reader).
 * Scalars are plain word-sized stores; the caption is guarded by a spinlock.
 */

typedef enum {
    TIMMY_MODE_BOOT = 0,
    TIMMY_MODE_IDLE,
    TIMMY_MODE_LISTENING,
    TIMMY_MODE_THINKING,
    TIMMY_MODE_SPEAKING,
    TIMMY_MODE_ERROR,
    TIMMY_MODE_OFF,       /* powering down */
    TIMMY_MODE_COUNT,
} timmy_mode_t;

typedef struct {
    int battery_pct;   /* -1 when no battery is connected */
    int battery_mv;    /* 0 when the board can't measure it */
    bool charging;
    bool usb;
} timmy_power_t;

void timmy_state_init(void);

void timmy_state_set_mode(timmy_mode_t mode);
timmy_mode_t timmy_state_mode(float *secs_in_mode);

/* Live audio level (mic while listening, playback while speaking), 0..1. */
void timmy_state_set_level(float level);
float timmy_state_level(void);

/* Progress of the current phase (recording length or playback), 0..1. */
void timmy_state_set_progress(float progress);
float timmy_state_progress(void);

/* Room for a page of reply text; longer captions are cut short. */
#define TIMMY_CAPTION_MAX 400

void timmy_state_set_caption(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the caption if it changed since *version; returns true on change. */
bool timmy_state_caption(char *out, size_t out_len, uint32_t *version);

/* A transient hint caption: cleared once TIMMY_HINT_MS passes. Plain
 * captions (replies, status) never expire; a hint set replaces any
 * caption, and any later plain caption cancels the pending expiry. */
#define TIMMY_HINT_MS 5000
void timmy_state_set_hint(const char *text);
/* Whether the caption is a hint that hasn't expired yet. */
bool timmy_state_hint_pending(void);

/* How much reply text the screen shows at once, set by the UI: lines of up to
 * `cols` characters. The replies are wrapped and paged to fit. A screen that
 * draws CJK larger than its other caption text sets a page for replies with CJK
 * in them too, and one with its accented letters' font (timmy_text_has_latin)
 * for replies with those; without one they get the usual page. */
void timmy_state_set_page(int cols, int lines);
void timmy_state_set_cjk_page(int cols, int lines);
void timmy_state_set_latin_page(int cols, int lines);
/* The page for `text`. */
void timmy_state_page(const char *text, int *cols, int *lines);

void timmy_state_set_power(const timmy_power_t *power);
timmy_power_t timmy_state_power(void);
/* A battery and no USB power (charger or computer). Power saving asleep (the
 * voice task resting, the display paused, the Wi-Fi nap) is only for then;
 * a change nudges (timmy_state_wait_awake). */
bool timmy_state_on_battery(void);
/* Bench tests over USB (">nap"): count as on battery until turned off. */
void timmy_state_set_as_if_battery(bool on);

/* Any user interaction: keeps Timmy awake. */
void timmy_state_poke(void);
float timmy_state_idle_secs(void);

/* Screen sleep (display dark, rendering paused). Waking also pokes. */
void timmy_state_set_asleep(bool asleep);
bool timmy_state_asleep(void);
/* For a task with nothing to do asleep: returns once awake, nudged
 * (timmy_state_nudge, which doesn't wake the screen) or timeout_ms passes. */
void timmy_state_wait_awake(uint32_t timeout_ms);
void timmy_state_nudge(void);

/* Touch/pet reaction. */
void timmy_state_make_happy(void);
float timmy_state_happiness(void);
