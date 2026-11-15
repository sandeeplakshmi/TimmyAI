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

#include "timmy_input.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_sleep.h"
#endif

#include "timmy_battery.h"
#include "timmy_ble.h"
#include "timmy_board.h"
#include "timmy_chat.h"
#include "timmy_console.h"
#include "timmy_link.h"
#include "timmy_mem.h"
#include "timmy_menu.h"
#include "timmy_settings.h"
#include "timmy_settings_ui.h"   /* LVGL only; e-paper builds use FreeInk */
#include "timmy_state.h"
#include "timmy_ui.h"
#include "timmy_voice.h"
#include "timmy_wifi.h"

static atomic_uint s_touch_talk_events;

void timmy_input_touch_talk(bool pressed)
{
    atomic_fetch_or(&s_touch_talk_events, pressed ? TIMMY_BTN_TALK_PRESS : TIMMY_BTN_TALK_RELEASE);
}

static const char *TAG = "timmy_input";

#define POLL_MS 10
#define REST_POLL_MS 50        /* screen off and paused: still quick to wake */
#define REST_WAIT_MS 1000      /* paused, buttons that interrupt: the rest waits this long */
#define NAP_WAIT_MS 10000      /* ... and Wi-Fi napping */
#define POWER_MS 2000          /* refresh battery */
#define REST_POWER_MS 10000    /* ... while paused */
#define WIFI_NAP_MS (2 * 60 * 1000)   /* low power this long: Wi-Fi off until it ends */
#define DOUBLE_TICKS 35        /* 350 ms: a second aux press within this toggles phone setup */

#define GOODBYE_MS 1500        /* let the goodbye animation play */
#define HINT_TICKS 60          /* 0.6 s: warn that holding powers off */
#define LONG_TICKS 150         /* 1.5 s: power off */
#define SLEEP_CHECK_MS 100

#define SERIAL_RX 1024         /* the driver drops what doesn't fit, so a console line must */
#define SERIAL_LINE 1024
#define CHAT_MAX (192 * 1024)  /* a typed message, assembled from "chat+=" lines */

static QueueHandle_t s_queue;
static bool (*s_console_hook)(const char *line);
static TaskHandle_t s_input;
static bool s_talk_down;
#define NOTE_TOGGLED() false
/* A push-to-talk press the voice task hasn't taken up yet: the speaker
 * button already ends the call it will start. Each such DOWN sets a mark of
 * its own and carries it; the voice task clears a mark only by that id, so
 * a press posted while it clears an older one keeps its mark. 0: none. */
static atomic_uint s_call_pending;
static atomic_uint s_mark_seq;
static atomic_uint s_call_epoch;
uint32_t timmy_input_call_epoch(void) { return atomic_load(&s_call_epoch); }
void timmy_input_clear_call_pending(uint32_t mark)
{
    if (mark) {
        atomic_compare_exchange_strong(&s_call_pending, &mark, 0);
    }
}
static bool s_cpu_low;      /* display stopped and the CPU allowed to sleep */
static volatile bool s_power_off_requested;
static bool s_cpu_pd;       /* timmy_input_power_init() holds the CPU's retention memory */
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_awake;   /* with s_cpu_pd: held whenever !s_cpu_low */
static bool s_awake_held;
#endif
static volatile bool s_nap_now;   /* ">nap": asleep, as if on battery, nap without waiting WIFI_NAP_MS */

static void post(timmy_ptt_t type, bool wake)
{
    timmy_input_event_t ev = { .type = type, .wake = wake, .epoch = timmy_input_call_epoch(),
                             .at_us = esp_timer_get_time() };
    bool pending = type == TIMMY_PTT_DOWN && timmy_voice_ptt_enabled() && !timmy_voice_in_call();
    if (pending) {
        uint32_t mark;
        do {
            mark = atomic_fetch_add(&s_mark_seq, 1) + 1;
        } while (!mark);
        ev.mark = mark;
        atomic_store(&s_call_pending, mark);   /* before the send, so the speaker button sees it */
    }
    ESP_LOGI(TAG, "PTT %s%s", type == TIMMY_PTT_DOWN ? "down" : type == TIMMY_PTT_UP ? "up" : "end call",
             wake ? " (waking)" : "");
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(20)) != pdTRUE) {
        /* A lost edge cannot leave a live microphone open. Invalidate the
         * entire gesture, including queued older DOWNs, without another send. */
        atomic_fetch_add(&s_call_epoch, 1);
        atomic_store(&s_call_pending, 0);
        ESP_LOGW(TAG, "input queue full: call gesture invalidated");
    }
}

static bool update_power(void);

static void power_off(void)
{
    if (!timmy_board->power_off) {
        return;   /* a physical switch powers it off */
    }
    ESP_LOGI(TAG, "shutting down");
    timmy_voice_shutdown(3000);   /* bounded best effort; never drop power before requesting end-call */
    timmy_state_set_asleep(false);
    update_power();
    timmy_state_set_progress(0);
    timmy_state_set_level(0);
    timmy_state_set_mode(TIMMY_MODE_OFF);
    timmy_state_set_caption("GOODBYE!");
    vTaskDelay(pdMS_TO_TICKS(GOODBYE_MS));
#if CONFIG_PM_ENABLE
    /* Automatic light sleep leaves the timer armed, and a deep sleep would
     * wake on it at once (with s_cpu_pd light sleep stays configured). */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
#endif
    esp_err_t err = timmy_board->power_off();
    /* Only reached if the board couldn't power off. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "power-off failed (%s)", esp_err_to_name(err));
    timmy_voice_cancel_shutdown();
    timmy_state_set_mode(TIMMY_MODE_IDLE);
    timmy_state_set_caption("COULDN'T POWER OFF");
}

static void set_asleep(bool asleep, const char *why)
{
    if (asleep && NOTE_TOGGLED()) {
        return;   /* never hide a recording note */
    }
    if (asleep != timmy_state_asleep()) {
        ESP_LOGI(TAG, "%s (%s)", asleep ? "sleeping" : "waking", why);
        timmy_state_set_asleep(asleep);
        if (s_input) {
            xTaskNotifyGive(s_input);   /* out of wait_buttons() */
        }
        if (!asleep && !timmy_wifi_connected()) {
            timmy_wifi_apply();   /* the screen says reconnecting: don't sit out the backoff */
        }
    }
}

static void toggle_phone_setup(void)
{
    bool on = !timmy_settings_ble_on();
    ESP_LOGI(TAG, "phone setup %s", on ? "on" : "off");
    timmy_settings_set_ble_on(on);
    timmy_ble_status_t b;
    timmy_ble_status(&b);
    if (on && b.name[0]) {
        /* The setup name stays up: it is the instruction for pairing. */
        timmy_state_set_caption("PHONE SETUP: %s", b.name);
    } else if (on) {
        timmy_state_set_caption("PHONE SETUP ON");
    } else {
        /* OFF is a confirmation, not a state worth keeping on screen. */
        timmy_state_set_hint("PHONE SETUP OFF");
    }
}

/*
 * Aux button: short press sleeps, a 1.5 s hold powers off, any press wakes.
 * Two quick presses toggle BLE phone setup, so the sleep waits a moment to
 * see whether a second press follows.
 */
static void aux_button(bool pressed, bool edge)
{
    static int held;
    static bool swallow;
    static bool hinted;
    static int sleep_in;    /* ticks until a pending single press sleeps */
    static char saved_caption[64];
    static bool saved_hint;

    if (sleep_in && --sleep_in == 0) {
        set_asleep(true, timmy_board->aux_button);
    }
    if (edge && pressed) {
        held = 0;
        hinted = false;
        swallow = timmy_state_asleep();
        if (swallow) {
            set_asleep(false, timmy_board->aux_button);
        } else if (sleep_in) {
            sleep_in = 0;
            swallow = true;
            toggle_phone_setup();
        }
        return;
    }
    if (pressed && !swallow) {
        if (!timmy_board->power_off) {
            return;   /* no hold to power off: a press only sleeps */
        }
        held++;
        if (held == HINT_TICKS) {
            uint32_t v = UINT32_MAX;
            timmy_state_caption(saved_caption, sizeof(saved_caption), &v);
            saved_hint = timmy_state_hint_pending();
            timmy_state_set_caption("HOLD TO POWER OFF");
            hinted = true;
        } else if (held == LONG_TICKS) {
            swallow = true;
            power_off();
        }
        return;
    }
    if (edge && !pressed && !swallow) {
        if (hinted) {
            /* Let go early: cancel, and put back what was there, a hint as a hint. */
            if (saved_hint) {
                timmy_state_set_hint(saved_caption);
            } else {
                timmy_state_set_caption("%s", saved_caption);
            }
        } else {
            sleep_in = DOUBLE_TICKS;
        }
    }
}

/* No touch: the aux button opens the menu and steps down it; any press wakes. */
static void menu_button(bool pressed, bool edge)
{
    if (!edge || !pressed) {
        return;
    }
    if (timmy_state_asleep()) {
        set_asleep(false, timmy_board->aux_button);
    } else if (!s_talk_down && !NOTE_TOGGLED()) {
        timmy_state_poke();
        timmy_menu_key(TIMMY_MENU_DOWN);
    }
}

static void aux_key(bool pressed, bool edge)
{
    if (timmy_board->touch) {
        aux_button(pressed, edge);
    } else {
        menu_button(pressed, edge);
    }
}

/* Talk button: push-to-talk, or Select while the menu is open. Asleep, the
 * press wakes and is posted as a waking one: timmy_voice records only if it's
 * still held once awake. A display-only board (no audio_init or mic_init)
 * runs no voice task to take presses off the queue, so there the press only
 * wakes, selects or confirms pairing. */
static void talk_button(unsigned ev)
{
    bool talk_down = s_talk_down;
    bool voice = timmy_board->audio_init || timmy_board->mic_init;
    static bool swallow;
    /* A quick tap can latch press and release in the same poll, and a release
     * can land just before the next press; keep them ordered. */
    bool released = ev & TIMMY_BTN_TALK_RELEASE;
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(TIMMY_PTT_UP, false);
        }
        talk_down = swallow = false;
        released = false;
    }
    if (!talk_down && !swallow && (ev & TIMMY_BTN_TALK_PRESS)) {
        if (timmy_link_talk_press()) {
            /* Confirmed a Timmy app pairing (Link's setup button). */
            timmy_state_poke();
            swallow = true;
        } else if (timmy_state_asleep()) {
            set_asleep(false, timmy_board->talk_button);
            if (voice) {
                post(TIMMY_PTT_DOWN, true);
                talk_down = true;
            } else {
                swallow = true;
            }
        } else if (timmy_menu_is_open()) {
            timmy_state_poke();
            if (!timmy_board->keyboard) timmy_menu_key(TIMMY_MENU_SELECT);
            swallow = true;
        } else if (voice) {
            post(TIMMY_PTT_DOWN, false);
            talk_down = true;
        } else {
            timmy_state_poke();
            swallow = true;
        }
    }
    if ((talk_down || swallow) && released) {
        if (talk_down) {
            post(TIMMY_PTT_UP, false);
        }
        talk_down = swallow = false;
    }
    s_talk_down = talk_down;
}

/* Keyboard menus don't repurpose Space/GO as Select. Navigation wakes the
 * screen without accidentally changing a setting; Enter can confirm pairing. */
static void keyboard_buttons(unsigned ev)
{
    const unsigned mask = TIMMY_BTN_UP | TIMMY_BTN_DOWN | TIMMY_BTN_LEFT |
                          TIMMY_BTN_RIGHT | TIMMY_BTN_ENTER | TIMMY_BTN_ESCAPE;
    if (!(ev & mask) || s_talk_down || NOTE_TOGGLED()) return;
    if ((ev & TIMMY_BTN_ENTER) && timmy_link_talk_press()) {
        timmy_state_poke();
        return;
    }
    if (timmy_state_asleep()) {
        set_asleep(false, "keyboard");
        return;
    }
    timmy_state_poke();
    if (ev & TIMMY_BTN_ESCAPE) timmy_menu_key(TIMMY_MENU_BACK);
    else if (ev & TIMMY_BTN_UP) timmy_menu_key(TIMMY_MENU_UP);
    else if (ev & TIMMY_BTN_DOWN) timmy_menu_key(TIMMY_MENU_DOWN);
    else if (ev & TIMMY_BTN_LEFT) timmy_menu_key(TIMMY_MENU_LEFT);
    else if (ev & TIMMY_BTN_RIGHT) timmy_menu_key(TIMMY_MENU_RIGHT);
    else if (ev & TIMMY_BTN_ENTER) timmy_menu_key(TIMMY_MENU_SELECT);
}


/* A pairing prompt wakes the screen and keeps it on; otherwise idle sleeps. */
static void check_sleep(void)
{
    timmy_ble_status_t ble;
    timmy_ble_status(&ble);
    bool prompt = ble.passkey || timmy_link_state() == TIMMY_LINK_CONFIRM;
    if (prompt) {
        set_asleep(false, "pairing");
        return;
    }
    if (timmy_voice_in_call()) return;
    int after = timmy_settings_sleep_s();
    float mode_t;
    if (after && !timmy_state_asleep() && timmy_state_mode(&mode_t) == TIMMY_MODE_IDLE
        && timmy_state_idle_secs() > after) {
        set_asleep(true, "auto-sleep");
    }
}

static void set_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = low ? CONFIG_XTAL_FREQ : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = low,
    };
    if (s_cpu_pd) {
        /* Light sleep stays configured: turning it off would free the
         * retention memory. The lock keeps the chip out of it instead, taken
         * before the full clock and let go only after the low one. */
        pm.light_sleep_enable = true;
        if (!low && !s_awake_held) {
            esp_pm_lock_acquire(s_awake);
            s_awake_held = true;
        }
    }
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
    if (s_cpu_pd && low && s_awake_held && err == ESP_OK) {
        esp_pm_lock_release(s_awake);
        s_awake_held = false;
    }
#else
    (void)low;
#endif
}

void timmy_input_power_init(void)
{
#if CONFIG_PM_ENABLE && CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP
    esp_err_t err = esp_sleep_cpu_retention_init();
    if (err == ESP_OK) {
        err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "timmy_awake", &s_awake);
    }
    if (err == ESP_OK) {
        esp_pm_lock_acquire(s_awake);
        s_awake_held = true;
        const esp_pm_config_t pm = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .light_sleep_enable = true,
        };
        err = esp_pm_configure(&pm);
    }
    s_cpu_pd = err == ESP_OK;
    if (!s_cpu_pd) {
        if (s_awake) {
            if (s_awake_held) {
                esp_pm_lock_release(s_awake);
                s_awake_held = false;
            }
            esp_pm_lock_delete(s_awake);
            s_awake = NULL;
        }
        esp_sleep_cpu_retention_deinit();
        ESP_LOGW(TAG, "CPU stays powered in light sleep: no retention memory (%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CPU powers down in light sleep: retention memory held");
    }
#endif
    timmy_battery_note_cpu_pd(s_cpu_pd);
}

bool timmy_input_cpu_pd(void)
{
    return s_cpu_pd;
}

/*
 * On battery with the screen dark and voice resting: stop the display and let
 * the CPU drop to the crystal clock and light-sleep between polls, or until a
 * button interrupts (wait_buttons). Returns true while the display is stopped.
 * With a USB host attached (">nap" on the bench) the CPU stays at full speed:
 * at the crystal clock a long line of serial output can stall the USB console
 * until the host reopens the port.
 */
static bool update_power(void)
{
    static bool paused;
    bool pause = timmy_board->display_pause && timmy_state_on_battery() && timmy_state_asleep()
                 && timmy_ui_dark() && timmy_voice_resting();
    bool want_low = pause && !timmy_console_host();
    if (pause == paused && want_low == s_cpu_low) {
        return paused;
    }
    if (pause && !paused) {
        timmy_board->display_pause(true);
    }
    if (want_low != s_cpu_low) {
        set_cpu_low(want_low);
    }
    if (!pause && paused) {
        timmy_board->display_pause(false);
    }
    paused = pause;
    s_cpu_low = want_low;
    ESP_LOGI(TAG, "%s", s_cpu_low ? "low power: display paused"
                        : paused  ? "display paused (USB host: CPU at full speed)"
                                  : "full power");
    return paused;
}

/*
 * Low power for WIFI_NAP_MS: Wi-Fi off, since keeping it associated costs
 * more than the rest of the chip. It rejoins when the screen wakes or USB
 * power arrives; meanwhile nothing reaches the device over the network.
 * Not while a voice note recorded offline waits to go (for a while). Returns
 * true while napping.
 */
static bool update_wifi_nap(TickType_t now, bool paused)
{
    static TickType_t low_since;
    static bool napping;
    if (!s_cpu_low) {
        low_since = now;
    }
    if (!timmy_state_asleep() && s_nap_now) {
        s_nap_now = false;
        timmy_state_set_as_if_battery(false);
    }
    /* Timmy keeps its socket open while it rests: with Wi-Fi off it drops
     * off the network, and a press right after waking finds no Timmy yet
     * and is only saved. Resting Wi-Fi (max modem sleep) still saves most
     * of the power; ">nap" on the bench still naps. */
    (void)low_since;
    bool nap = paused && s_nap_now;
    if (nap != napping) {
        napping = nap;
        ESP_LOGI(TAG, "Wi-Fi %s", nap ? "napping" : "waking");
        timmy_wifi_nap(nap);
    }
    return napping;
}

/* Independent POWER never goes through PTT, AUX's menu or its setup double
 * click. LONG is supplied by the board only after a validated hold. */
static bool s_power_down, s_power_swallow;

static void power_release(void)
{
    if (!s_power_down) return;
    s_power_down = false;
    if (!s_power_swallow) set_asleep(true, timmy_board->power_button);
    s_power_swallow = false;
}

static void dedicated_buttons(unsigned ev)
{
    bool release = (ev & TIMMY_BTN_POWER_RELEASE) != 0;
    /* Preserve release-before-new-press ordering for latched boards. */
    if (s_power_down && release) {
        power_release();
        release = false;
    }
    if (!s_power_down && (ev & TIMMY_BTN_POWER_PRESS)) {
        s_power_down = true;
        s_power_swallow = timmy_state_asleep();
        timmy_state_poke();
        if (s_power_swallow) set_asleep(false, timmy_board->power_button);
    }
    if (s_power_down && !s_power_swallow && (ev & TIMMY_BTN_POWER_LONG)) {
        s_power_swallow = true;   /* no sleep on release after shutdown/failure */
        if (s_talk_down) talk_button(TIMMY_BTN_TALK_RELEASE);
        power_off();
    }
    if (release) power_release();
    if (ev & TIMMY_BTN_SPEAKER_PRESS) {
        timmy_state_poke();
        if (timmy_state_asleep()) {
            set_asleep(false, timmy_board->speaker_button);   /* wake-only press */
        } else if (timmy_state_mode(NULL) != TIMMY_MODE_OFF) {
            bool on = !timmy_settings_speaker_on();
            timmy_settings_set_speaker_on(on);
            if (timmy_state_mode(NULL) == TIMMY_MODE_IDLE) {
                timmy_state_set_hint(on ? "SPEAKER ON" : "SPEAKER OFF");
            }
        }
    }
}

static void input_task(void *arg)
{
    (void)arg;
    bool aux_down = false;
    bool paused = false;
    TickType_t checked = xTaskGetTickCount() - pdMS_TO_TICKS(SLEEP_CHECK_MS);
    TickType_t powered = xTaskGetTickCount() - pdMS_TO_TICKS(POWER_MS);

    for (;;) {
        unsigned ev = timmy_board->poll_buttons() | atomic_exchange(&s_touch_talk_events, 0);
        if (ev & (TIMMY_BTN_TALK_PRESS | TIMMY_BTN_TALK_RELEASE)) {
            ESP_LOGI(TAG, "talk key:%s%s", ev & TIMMY_BTN_TALK_PRESS ? " press" : "",
                     ev & TIMMY_BTN_TALK_RELEASE ? " release" : "");
            talk_button(ev);
        }
        keyboard_buttons(ev);
        dedicated_buttons(ev);
        /* A latched key (the 1.75's PMU) can report press and release in the
         * same poll, and a release can land just before the next press; keep
         * them ordered, as talk_button does. */
        bool aux_press = ev & TIMMY_BTN_AUX_PRESS;
        bool aux_release = ev & TIMMY_BTN_AUX_RELEASE;
        bool aux_edge = false;
        if (aux_down && aux_release) {
            aux_down = false;
            aux_release = false;
            aux_edge = true;
            aux_key(false, true);
        }
        if (!aux_down && aux_press) {
            aux_down = aux_edge = true;
            aux_key(true, true);
            if (aux_release) {
                aux_down = false;
                aux_key(false, true);
            }
        }
        if (!aux_edge) {
            aux_key(aux_down, false);
        }

        if (s_power_off_requested) {
            s_power_off_requested = false;
            power_off();
        }

        TickType_t now = xTaskGetTickCount();
        if (now - checked >= pdMS_TO_TICKS(SLEEP_CHECK_MS)) {
            checked = now;
            check_sleep();
        }

        if (now - powered >= pdMS_TO_TICKS(paused ? REST_POWER_MS : POWER_MS)) {
            powered = now;
            timmy_power_t p = { .battery_pct = -1 };
            if (timmy_board->read_power && timmy_board->read_power(&p) == ESP_OK) {
                timmy_state_set_power(&p);
                timmy_battery_note_power(&p, timmy_state_on_battery());
            }
        }

        paused = update_power();
        bool napping = update_wifi_nap(now, paused);
        timmy_battery_note_state(timmy_state_asleep(), s_cpu_low);
        /* Paused, anything but a button (a pairing prompt, an image) is
         * noticed within REST_WAIT_MS. Napping, nothing comes over the
         * network; USB power arriving is noticed within NAP_WAIT_MS. */
        if (paused && timmy_board->wait_buttons) {
            timmy_board->wait_buttons(napping ? NAP_WAIT_MS : REST_WAIT_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(paused ? REST_POLL_MS : POLL_MS));
        }
    }
}

/* Reads the rest of a line into buf; false if it was too long (the rest is dropped). */
static bool read_line(char *buf, size_t cap)
{
    size_t len = 0;
    bool whole = true;
    uint8_t c;
    while (timmy_console_getc(&c) && c != '\n') {
        if (c == '\r') {
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = (char)c;
        } else {
            whole = false;
        }
    }
    buf[len] = '\0';
    return whole;
}

#define CHAT_OVER_SERIAL "true"

/*
 * "chat+=TEXT" adds a piece of a message and "chat=TEXT" adds the last piece
 * and sends it (TEXT escaped: \n \r \t \\). Every line is acknowledged, and the
 * host waits for that before the next: the driver drops what it has no room for.
 */
static char *s_chat;        /* the message so far */
static size_t s_chat_len;

static void chat_line(char *piece, bool last, bool whole)
{
    size_t n = timmy_hatch_unescape(piece);
    if (!s_chat) {
        s_chat = heap_caps_malloc(CHAT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_chat_len = 0;
    }
    const char *err = !whole ? "LINE TOO LONG" : !s_chat ? "OUT OF MEMORY" : s_chat_len + n >= CHAT_MAX ? "TOO LONG" : NULL;
    if (err) {
        free(s_chat);
        s_chat = NULL;
        timmy_hatch_console("error", err, NULL);
        return;
    }
    memcpy(s_chat + s_chat_len, piece, n);
    s_chat_len += n;
    s_chat[s_chat_len] = '\0';
    timmy_hatch_console("ack", NULL, "\"bytes\":%u", (unsigned)s_chat_len);
    if (last) {
        timmy_hatch_text_turn(s_chat);   /* frees it */
        s_chat = NULL;
    }
}

/* Drops a half-sent message and ends a typed turn. */
static void chat_cancel(void)
{
    free(s_chat);
    s_chat = NULL;
    timmy_hatch_text_cancel();
}

/*
 * Bench: puts the face in a mode ("face=thinking") until the voice path or
 * another "face=" moves it on. "face=happy" goes back to idle with the happy
 * hop, as a finished turn does.
 */
static void set_face(const char *name)
{
    static const char *const modes[TIMMY_MODE_COUNT] = {
        [TIMMY_MODE_BOOT] = "boot",
        [TIMMY_MODE_IDLE] = "idle",
        [TIMMY_MODE_LISTENING] = "listening",
        [TIMMY_MODE_THINKING] = "thinking",
        [TIMMY_MODE_SPEAKING] = "speaking",
        [TIMMY_MODE_ERROR] = "error",
        [TIMMY_MODE_OFF] = "off",
    };
    timmy_state_poke();
    bool happy = !strcmp(name, "happy");
    if (happy) {
        name = "idle";   /* where a finished turn hops to */
    }
    for (int m = 0; m < TIMMY_MODE_COUNT; m++) {
        if (!strcmp(name, modes[m])) {
            timmy_state_set_mode(TIMMY_MODE_IDLE);   /* only idle leaves "off" */
            timmy_state_set_mode((timmy_mode_t)m);
            if (happy) {
                timmy_state_make_happy();
            }
            return;
        }
    }
    printf("@face.error unknown face \"%s\": boot idle listening thinking speaking error off happy\n", name);
    fflush(stdout);
}

/*
 * Console-only commands; false for setup commands. Their buffers are taken
 * per command: without PSRAM, static ones would hold internal RAM for good.
 */
static bool console_command(char *line, bool whole)
{
    if (whole && s_console_hook && s_console_hook(line)) {
        return true;
    }
    if (whole && timmy_server_console(line)) {
        return true;
    }
    if (!strcmp(line, "status")) {
        size_t cap = 1024;   /* long SSID, host and VM names escaped: past 512 */
        char *json = heap_caps_malloc(cap, TIMMY_BIG_CAPS);
        if (json) {
            timmy_ble_status_json(json, cap);
            printf("@status {\"board\":\"%s\",\"chat\":%s,\"device\":%s}\n", timmy_board->name,
                   CHAT_OVER_SERIAL, json);
            fflush(stdout);
            free(json);
        }
        return true;
    }
    bool reset = !strcmp(line, "power.reset");
    if (reset || !strcmp(line, "power")) {
        if (reset) {
            timmy_battery_reset();
        }
        const char *saved = timmy_battery_saved_json();
        if (saved) {
            printf("@power.saved %s\n", saved);
        }
        size_t cap = 2048;   /* two dozen power locks */
        char *json = heap_caps_malloc(cap, TIMMY_BIG_CAPS);
        if (json) {
            timmy_battery_json(json, cap);
            printf("@power %s\n", json);
            free(json);
        }
        fflush(stdout);
        return true;
    }
    if (!strcmp(line, "nap")) {
        timmy_state_set_as_if_battery(true);
        set_asleep(true, "serial");
        s_nap_now = true;
        return true;
    }
#if LV_USE_SNAPSHOT
    if (!strcmp(line, "ui.demo=1") || !strcmp(line, "ui.demo=0")) {
        timmy_ui_bench_demo(!strcmp(line, "ui.demo=1"));
        return true;
    }
    if (!strcmp(line, "ui=settings") || !strcmp(line, "ui=face")) {
        timmy_ui_bench_page(!strcmp(line, "ui=settings"));
        return true;
    }
    if (!strncmp(line, "ui.settings=", 12)) {
        if (timmy_settings_ui_bench_open(line + 12)) {
            timmy_ui_bench_page(true);
        } else {
            printf("@ui.error unknown settings page \"%s\": home wifi timmy ble sound sleep battery power\n", line + 12);
            fflush(stdout);
        }
        return true;
    }
#endif
    if (!strncmp(line, "face=", 5)) {
        set_face(line + 5);
        return true;
    }
    if (strncmp(line, "chat", 4) != 0) {
        return false;
    }
    if (!strcmp(line, "chat.cancel")) {
        chat_cancel();
        return true;
    }
    bool last = !strncmp(line, "chat=", 5);
    if (last || !strncmp(line, "chat+=", 6)) {
        chat_line(line + (last ? 5 : 6), last, whole);
        return true;
    }
    return false;
}

/*
 * Bench testing over the USB cable: 'd' / 'u' act as the talk button going
 * down / up, so the voice path can be driven without a finger on the button;
 * 'm' plays a built-in MP3 through the reply decoder; 'a' / 's' press the
 * menu's Down / Select; 'p' sends a screenshot; 'z' / 'w' sleep and wake.
 * A line starting with '>' is a setup command, the same "key=value" text as
 * the BLE CMD characteristic, or one of the console's own: "status" prints
 * the device's state, "power" the battery meter (timmy_battery.h) and
 * "power.reset" starts it over, "nap" sleeps and leaves Wi-Fi at once (as
 * two minutes asleep on battery would; 'w' rejoins), "face=" shows a face
 * (see set_face), and "chat=" sends a typed message to Hatch (see chat_line
 * and tools/timmy/chat.py).
 */
static void serial_task(void *arg)
{
    if (timmy_console_install(SERIAL_RX) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        uint8_t c;
        if (!timmy_console_getc(&c)) {
            continue;
        }
        if (c == 'm') {
            timmy_voice_request_mp3test();
        } else if (c == 'a' || c == 's') {
            timmy_state_poke();
            timmy_menu_key(c == 'a' ? TIMMY_MENU_DOWN : TIMMY_MENU_SELECT);
        } else if (c == 'p') {
            timmy_ui_request_snapshot();
        } else if (c == 'z' || c == 'w') {
            set_asleep(c == 'z', "serial");
        } else if (c == '>') {
            char *line = heap_caps_malloc(SERIAL_LINE, TIMMY_BIG_CAPS);
            char none[1];   /* no room: the line is read and dropped */
            bool whole = read_line(line ? line : none, line ? SERIAL_LINE : sizeof(none));
            if (line && !console_command(line, whole)) {
                timmy_ble_command(line);
            }
            free(line);
        } else if (c == 'd' || c == 'u') {
            timmy_state_poke();
            post(c == 'd' ? TIMMY_PTT_DOWN : TIMMY_PTT_UP, false);
        }
    }
}

void timmy_input_set_console_hook(bool (*hook)(const char *line))
{
    s_console_hook = hook;
}

esp_err_t timmy_input_start(QueueHandle_t queue)
{
    s_queue = queue;
    if (xTaskCreate(input_task, "timmy_input", 4096, NULL, 6, &s_input) != pdPASS) {
        return ESP_FAIL;
    }
    /* Bench-test and setup console; the input still works if it can't start. */
    xTaskCreate(serial_task, "timmy_serial", 3584, NULL, 5, NULL);
    return ESP_OK;
}

void timmy_input_request_power_off(void)
{
    s_power_off_requested = true;
}
