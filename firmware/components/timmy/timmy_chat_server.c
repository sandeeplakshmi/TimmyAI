/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */
/*
 * Timmy mode (CONFIG_TIMMY_SERVER): the timmy_chat.h push-to-talk API, backed by
 * a WebSocket to Timmy, a personal assistant server you run yourself.
 *
 * The voice task (timmy_voice.c) runs a push-to-talk turn:
 * turn_begin on press, turn_audio while held, turn_end on release, then
 * turn_event / turn_read / turn_caption to play the reply. Here:
 *   - the board keeps one WebSocket open to timmy.url (ws:// or wss://, with
 *     the certificate bundle) while Wi-Fi is up, reconnecting on its own;
 *   - the server's hello carries a nonce; the board answers with
 *     HMAC-SHA256(device key, nonce), so the key never goes over the network;
 *   - mic audio goes up as binary frames of 16 kHz int16 PCM from a PSRAM
 *     ring, sent by this file's task so a slow network never stalls capture;
 *   - the reply comes back as {"type":"say","text","frames"} then that many
 *     frames of 16 kHz PCM, sentence by sentence, into a PSRAM ring the
 *     voice task plays from. Captions follow the sentence being played.
 * Setup over USB serial (timmy_server_console), kept in NVS namespace "timmy":
 *   >timmy.url=ws://<server-ip>:8790/gadget   >timmy.host=<server-ip> (same, port 8790)
 *   >timmy.key=<64 hex>                      >timmy.device=timmy-desk   >timmy (status)
 *   >timmy.fallback=wss://<host>/gadget      (used when the home address doesn't answer)
 */

#include "timmy_chat.h"
#include "timmy_chat_priv.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "timmy_pixel.h"
#include "timmy_state.h"
#include "timmy_wifi.h"

static const char *TAG = "timmy";

#define URL_MAX 160
#define DEV_MAX 32
#define KEY_BYTES 32
#define MIC_RING (8 * 16000 * 2)      /* 8 s of mic backlog */
#define SPK_RING (24 * 16000 * 2)     /* 24 s of reply speech */
#define UP_PIECE 1280                 /* 40 ms per binary frame */
#define TEXT_MAX 1024
#define SAY_MAX 24
#define EV_TEXT 96
#define RX_MAX 4096
#define REPLY_TIMEOUT_US (90LL * 1000000)
#define DEFAULT_PORT 8790

typedef enum { P_IDLE, P_TALKING, P_WAITING, P_REPLY } phase_t;

typedef struct {
    timmy_hatch_ev_t type;
    char text[EV_TEXT];
} ev_t;

typedef struct {
    uint16_t off, len;        /* in s_text */
    uint32_t start, frames;   /* in reply frames */
} say_t;

static nvs_handle_t s_nvs;
static char s_url[URL_MAX + 1];
/* Fallback server URL (wss://), used when the home address doesn't answer.
 * Set over serial (>timmy.fallback=) or sent by the server
 * ({"type":"fallback"}), and kept in NVS; the board learns a changed one the
 * next time it connects. */
static char s_fallback[URL_MAX + 1];
static volatile bool s_away;            /* this connection uses the fallback URL */
static int64_t s_unready_since;         /* trying to connect since (0: connected or idle) */
#define SWITCH_AFTER_US (12LL * 1000000)
#define ALIVE_US (5LL * 1000000)    /* a ping after waking must come back within this (radio and CPU may still be waking) */
static char s_dev[DEV_MAX + 1] = "timmy-desk";
static uint8_t s_key[KEY_BYTES];
static bool s_have_key;

static esp_websocket_client_handle_t s_ws;
static volatile bool s_ready;          /* authenticated */
static volatile bool s_restart;        /* settings changed: reconnect */
static char s_status[48] = "";

static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;       /* the turn's text and sentence table */
static StreamBufferHandle_t s_mic, s_spk;
static volatile phase_t s_phase;
static volatile bool s_end_pending;    /* release: send "end" once the mic ring drains */
static volatile bool s_begin_pending;
static volatile bool s_cancel_pending;
static volatile bool s_got_done;
static int64_t s_t_end;

static char s_text[TEXT_MAX];
static size_t s_text_len;
static say_t s_say[SAY_MAX];
static int s_n_say;
static uint32_t s_frames_total;
static uint32_t s_say_left;            /* frames still due for the newest "say" */
static uint8_t s_odd;                  /* a byte left from an odd-sized binary frame */
static bool s_has_odd;

static char *s_rx;                     /* a text frame gathered across fragments */
static size_t s_rx_len;

/* ---- Settings ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_key(const char *hex, uint8_t out[KEY_BYTES])
{
    if (strlen(hex) != KEY_BYTES * 2) return false;
    for (int i = 0; i < KEY_BYTES; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

static void load_settings(void)
{
    size_t n = sizeof(s_url);
    if (nvs_get_str(s_nvs, "url", s_url, &n) != ESP_OK) s_url[0] = '\0';
    n = sizeof(s_fallback);
    if (nvs_get_str(s_nvs, "fallback", s_fallback, &n) != ESP_OK) s_fallback[0] = '\0';
    n = sizeof(s_dev);
    if (nvs_get_str(s_nvs, "device", s_dev, &n) != ESP_OK) strlcpy(s_dev, "timmy-desk", sizeof(s_dev));
    n = sizeof(s_key);
    s_have_key = nvs_get_blob(s_nvs, "key", s_key, &n) == ESP_OK && n == KEY_BYTES;
}

static bool configured(void)
{
    return s_url[0] && s_have_key;
}

static void report(timmy_hatch_state_t st, const char *detail);

bool timmy_server_console(const char *line)
{
    if (strncmp(line, "timmy", 5) != 0 || (line[5] && line[5] != '.')) return false;
    if (!s_nvs) {
        printf("@timmy error not started\n");
        return true;
    }
    const char *eq = strchr(line, '=');
    const char *val = eq ? eq + 1 : "";
    size_t klen = eq ? (size_t)(eq - line) : strlen(line);
    bool changed = false;
    if (klen == 5) {
        printf("@timmy {\"url\":\"%s\",\"fallback\":\"%s\",\"device\":\"%s\",\"key\":%s,\"connected\":%s,\"status\":\"%s\"}\n", s_url,
               s_fallback, s_dev, s_have_key ? "true" : "false", s_ready ? "true" : "false", s_status);
    } else if (klen == 9 && !strncmp(line, "timmy.url", 9)) {
        if (strncmp(val, "ws://", 5) && strncmp(val, "wss://", 6)) {
            printf("@timmy.url error must start with ws:// or wss://\n");
        } else if (strlen(val) > URL_MAX) {
            printf("@timmy.url error too long\n");
        } else {
            nvs_set_str(s_nvs, "url", val);
            changed = true;
            printf("@timmy.url ok %s\n", val);
        }
    } else if (klen == 10 && !strncmp(line, "timmy.host", 10)) {
        char url[URL_MAX + 1];
        if (!*val || strchr(val, '/') || snprintf(url, sizeof(url), strchr(val, ':') ? "ws://%s/gadget" :
                                                  "ws://%s:%d/gadget", val, DEFAULT_PORT) >= (int)sizeof(url)) {
            printf("@timmy.host error give a host or host:port\n");
        } else {
            nvs_set_str(s_nvs, "url", url);
            changed = true;
            printf("@timmy.url ok %s\n", url);
        }
    } else if (klen == 9 && !strncmp(line, "timmy.key", 9)) {
        uint8_t key[KEY_BYTES];
        if (!parse_key(val, key)) {
            printf("@timmy.key error want 64 hex characters\n");
        } else {
            nvs_set_blob(s_nvs, "key", key, sizeof(key));
            memset(key, 0, sizeof(key));
            changed = true;
            printf("@timmy.key ok (stored, not shown)\n");
        }
    } else if (klen == 14 && !strncmp(line, "timmy.fallback", 14)) {
        if (*val && (strncmp(val, "wss://", 6) || strlen(val) > URL_MAX)) {
            printf("@timmy.fallback error must be a wss:// URL of up to %d characters (empty clears it)\n", URL_MAX);
        } else {
            strlcpy(s_fallback, val, sizeof(s_fallback));
            if (*val) nvs_set_str(s_nvs, "fallback", val); else nvs_erase_key(s_nvs, "fallback");
            changed = true;
            printf("@timmy.fallback ok %s\n", *val ? val : "(cleared)");
        }
    } else if (klen == 12 && !strncmp(line, "timmy.device", 12)) {
        if (!*val || strlen(val) > DEV_MAX) {
            printf("@timmy.device error 1-%d characters\n", DEV_MAX);
        } else {
            nvs_set_str(s_nvs, "device", val);
            changed = true;
            printf("@timmy.device ok %s\n", val);
        }
    } else {
        printf("@timmy error unknown: timmy.url= timmy.host= timmy.key= timmy.device= timmy\n");
    }
    if (changed) {
        nvs_commit(s_nvs);
        load_settings();
        report(configured() ? TIMMY_HATCH_UNTESTED : TIMMY_HATCH_NOT_SET, "");
        s_restart = true;
    }
    fflush(stdout);
    return true;
}

/* ---- Status ---- */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static timmy_hatch_state_t s_state = TIMMY_HATCH_UNTESTED;

static void report(timmy_hatch_state_t st, const char *detail)
{
    portENTER_CRITICAL(&s_mux);
    s_state = st;
    strlcpy(s_status, detail, sizeof(s_status));
    portEXIT_CRITICAL(&s_mux);
}

void timmy_hatch_status(timmy_hatch_status_t *out)
{
    out->vm_id[0] = '\0';
    if (!configured()) {
        out->state = TIMMY_HATCH_NOT_SET;
        strlcpy(out->detail, "Set timmy.url and timmy.key over USB", sizeof(out->detail));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    out->state = s_state;
    strlcpy(out->detail, s_status, sizeof(out->detail));
    portEXIT_CRITICAL(&s_mux);
    if (!timmy_wifi_connected()) {
        out->state = TIMMY_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (s_ready) {
        out->state = TIMMY_HATCH_REACHABLE;
        strlcpy(out->detail, "Timmy", sizeof(out->detail));
    } else if (!out->detail[0]) {
        out->state = TIMMY_HATCH_TESTING;
        strlcpy(out->detail, "Connecting to Timmy...", sizeof(out->detail));
    }
}

void timmy_hatch_test(void) { s_restart = true; }
void timmy_hatch_config_changed(void) { s_restart = true; }
/* Waking from a battery rest: the socket may have died while the radio slept
 * (routers drop frames buffered for long-sleeping clients). Check it at once
 * with a ping instead of finding out on the next press. */
static volatile bool s_check_alive;
static volatile int64_t s_pong_at;
void timmy_hatch_set_resting(bool resting)
{
    if (!resting) s_check_alive = true;
}

const char *timmy_hatch_state_name(timmy_hatch_state_t state)
{
    switch (state) {
    case TIMMY_HATCH_NOT_SET: return "Not set up";
    case TIMMY_HATCH_OFFLINE: return "Offline";
    case TIMMY_HATCH_UNTESTED: return "Saved";
    case TIMMY_HATCH_TESTING: return "Connecting";
    case TIMMY_HATCH_REACHABLE: return "Connected";
    case TIMMY_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

/* Timmy's own pieces that other files may reach for. */
bool timmy_hatch_configured(void) { return configured(); }
void timmy_hatch_report(timmy_hatch_state_t state, const char *detail) { report(state, detail); }
void timmy_hatch_report_vm(const char *vm_id) { (void)vm_id; }
void timmy_hatch_chat_connect(void) { s_restart = true; }
void timmy_hatch_chat_forget(void) { s_restart = true; }

/* ---- Events ---- */

static void emit(timmy_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type };
    if (text) strlcpy(ev.text, text, sizeof(ev.text));
    if (xQueueSend(s_events, &ev, 0) != pdTRUE) {
        ev_t old;
        xQueueReceive(s_events, &old, 0);   /* keep the newest */
        xQueueSend(s_events, &ev, 0);
    }
}

static void fail(const char *why)
{
    if (s_phase == P_IDLE) return;
    s_phase = P_IDLE;
    s_end_pending = false;
    emit(TIMMY_HATCH_EV_ERROR, why);
}

/* ---- The WebSocket ---- */

static bool send_json(const char *json)
{
    return s_ws && esp_websocket_client_is_connected(s_ws) &&
           esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(1000)) >= 0;
}

static void on_hello(const char *nonce)
{
    uint8_t mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, s_key, KEY_BYTES, (const uint8_t *)nonce, strlen(nonce), mac) != 0) {
        report(TIMMY_HATCH_UNREACHABLE, "HMAC failed");
        return;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    char msg[256];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"device\":\"%s\",\"mac\":\"%s\",\"fw\":\"%s\"}", s_dev, hex,
             esp_app_get_description()->version);
    send_json(msg);
}

static void on_say(const char *text, uint32_t frames)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = strlen(text);
    if (s_text_len && s_text_len + 1 < sizeof(s_text)) s_text[s_text_len++] = ' ';
    if (s_text_len + len >= sizeof(s_text)) len = sizeof(s_text) - 1 - s_text_len;
    if (s_n_say < SAY_MAX) {
        s_say[s_n_say++] = (say_t){ .off = (uint16_t)s_text_len, .len = (uint16_t)len,
                                    .start = s_frames_total, .frames = frames ? frames : 1 };
    } else {
        s_say[SAY_MAX - 1].len = (uint16_t)(s_text_len + len - s_say[SAY_MAX - 1].off);   /* merge the tail */
        s_say[SAY_MAX - 1].frames += frames;
    }
    memcpy(s_text + s_text_len, text, len);
    s_text_len += len;
    s_text[s_text_len] = '\0';
    s_frames_total += frames;
    s_say_left = frames;
    xSemaphoreGive(s_lock);
}

/* ---- Looks from the server's skills: {"type":"eyes","look":...} ---- */

#define LOOK_SLOTS 4
typedef struct {
    char tag[24];         /* "" = a passing look */
    int look, particles;
    float hue, secs;      /* hue -1: unchanged; secs 0: until cleared */
    float ring_total, ring_left;
    int64_t at_us;
} look_slot_t;
static look_slot_t s_looks[LOOK_SLOTS + 1];   /* [0]: the passing look, then tagged ones */
static portMUX_TYPE s_look_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile float s_pout_after = 4 * 3600;

static int name_index(const char *name, const char *const *names, int n)
{
    for (int i = 0; name && i < n; i++) {
        if (names[i] && !strcmp(name, names[i])) return i;
    }
    return 0;
}

static void on_eyes(cJSON *j)
{
    static const char *const LOOKS[TIMMY_LOOK_COUNT] = {
        "none", "happy", "cheer", "surprised", "squint", "droop", "shiver", "calm", "glow", "focus", "sleepy",
        "asleep", "pout",
    };
    static const char *const PARTS[TIMMY_PARTICLES_COUNT] = {
        "none", "sun", "rain", "snow", "hearts", "stars", "confetti", "z", "drops",
    };
    const char *look = cJSON_GetStringValue(cJSON_GetObjectItem(j, "look"));
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tag"));
    cJSON *pa = cJSON_GetObjectItem(j, "pout_after");
    if (cJSON_IsNumber(pa)) s_pout_after = (float)pa->valuedouble;
    if (look && !strcmp(look, "settings")) return;
    look_slot_t v = {
        .look = name_index(look, LOOKS, TIMMY_LOOK_COUNT),
        .particles = name_index(cJSON_GetStringValue(cJSON_GetObjectItem(j, "particles")), PARTS, TIMMY_PARTICLES_COUNT),
        .hue = cJSON_IsNumber(cJSON_GetObjectItem(j, "hue")) ? (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "hue")) : -1,
        .secs = cJSON_IsNumber(cJSON_GetObjectItem(j, "secs")) ? (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "secs")) : 8,
        .ring_total = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "ring_total")),
        .ring_left = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "ring_left")),
        .at_us = esp_timer_get_time(),
    };
    if (v.ring_total != v.ring_total) v.ring_total = 0;   /* NaN: none */
    if (v.ring_left != v.ring_left) v.ring_left = 0;
    if (look && !v.look && strcmp(look, "none")) return;   /* a look this board doesn't know */
    portENTER_CRITICAL(&s_look_mux);
    if (!tag || v.secs > 0) {
        s_looks[0] = v;
    } else {
        int slot = -1, free_slot = -1;
        for (int i = 1; i <= LOOK_SLOTS; i++) {
            if (!strcmp(s_looks[i].tag, tag)) slot = i;
            if (!s_looks[i].tag[0] && free_slot < 0) free_slot = i;
        }
        if (slot < 0) slot = free_slot;
        if (slot > 0) {
            if (v.look) {
                strlcpy(v.tag, tag, sizeof(v.tag));
                s_looks[slot] = v;
            } else {
                memset(&s_looks[slot], 0, sizeof(s_looks[slot]));   /* cleared */
            }
        }
    }
    portEXIT_CRITICAL(&s_look_mux);
}

bool timmy_timmy_look(int *look, float *t, float *hue, int *particles, float *ring, float *pout_after)
{
    int64_t now = esp_timer_get_time();
    look_slot_t v = { 0 };
    portENTER_CRITICAL(&s_look_mux);
    if (s_looks[0].look && (now - s_looks[0].at_us) / 1e6f < s_looks[0].secs) {
        v = s_looks[0];
    } else {
        for (int i = 1; i <= LOOK_SLOTS; i++) {   /* the newest kept look */
            if (s_looks[i].look && (!v.look || s_looks[i].at_us > v.at_us)) v = s_looks[i];
        }
    }
    portEXIT_CRITICAL(&s_look_mux);
    *pout_after = s_pout_after;
    if (!v.look) return false;
    float since = (now - v.at_us) / 1e6f;
    *look = v.look;
    *t = since;
    *hue = v.hue;
    *particles = v.particles;
    *ring = v.ring_total > 0 ? fmaxf(0, (v.ring_left - since) / v.ring_total) : -1;
    return true;
}

/* ---- Timmy speaking unasked (a reminder, a nudge): {"type":"push"} ---- */

static void reset_turn(void);

static volatile bool s_incoming, s_push_silent;

/* The reply's mood, for the avatar: set by the server with each sentence. */
static volatile int s_mood;
static volatile int64_t s_mood_us;

static void set_mood(const char *name)
{
    static const char *const NAMES[TIMMY_MOOD_COUNT] = {
        [TIMMY_MOOD_NONE] = "none", [TIMMY_MOOD_HAPPY] = "happy", [TIMMY_MOOD_EXCITED] = "excited",
        [TIMMY_MOOD_CURIOUS] = "curious", [TIMMY_MOOD_SAD] = "sad", [TIMMY_MOOD_LOVE] = "love",
        [TIMMY_MOOD_LAUGHING] = "laughing", [TIMMY_MOOD_SURPRISED] = "surprised",
    };
    if (!name) return;
    for (int i = 0; i < TIMMY_MOOD_COUNT; i++) {
        if (!strcmp(name, NAMES[i])) {
            if (i != s_mood) ESP_LOGI(TAG, "mood %s", name);
            s_mood_us = esp_timer_get_time();
            s_mood = i;
            return;
        }
    }
}

int timmy_timmy_mood(float *age_s)
{
    int m = s_mood;
    if (age_s) *age_s = (float)(esp_timer_get_time() - s_mood_us) / 1e6f;
    return m;
}

static void on_text(const char *data, size_t len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    if (!j) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
    if (!type) {
        /* nothing */
    } else if (!strcmp(type, "hello")) {
        const char *nonce = cJSON_GetStringValue(cJSON_GetObjectItem(j, "nonce"));
        if (nonce) on_hello(nonce);
    } else if (!strcmp(type, "pong")) {
        s_pong_at = esp_timer_get_time();
    } else if (!strcmp(type, "ready")) {
        s_ready = true;
        s_unready_since = 0;
        report(TIMMY_HATCH_REACHABLE, s_away ? "Away (fallback server)" : "Home (Wi-Fi)");
        ESP_LOGI(TAG, "connected to Timmy as %s (%s)", s_dev, s_away ? "away, fallback" : "home");
    } else if (!strcmp(type, "heard")) {
        if (s_phase == P_WAITING || s_phase == P_REPLY) emit(TIMMY_HATCH_EV_HEARD, text ? text : "");
    } else if (!strcmp(type, "say")) {
        if (s_phase == P_WAITING || s_phase == P_REPLY) {
            s_phase = P_REPLY;
            double frames = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "frames"));
            set_mood(cJSON_GetStringValue(cJSON_GetObjectItem(j, "mood")));
            on_say(text ? text : "", frames > 0 ? (uint32_t)frames : 0);
            emit(TIMMY_HATCH_EV_REPLY, text ? text : "");
        }
    } else if (!strcmp(type, "done")) {
        if (s_phase == P_WAITING || s_phase == P_REPLY) {
            s_got_done = true;
            s_phase = P_IDLE;
            emit(TIMMY_HATCH_EV_DONE, NULL);
        }
    } else if (!strcmp(type, "fallback")) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
        if (url && !strncmp(url, "wss://", 6) && strlen(url) <= URL_MAX && strcmp(url, s_fallback)) {
            strlcpy(s_fallback, url, sizeof(s_fallback));
            nvs_set_str(s_nvs, "fallback", s_fallback);
            nvs_commit(s_nvs);
            ESP_LOGI(TAG, "fallback address: %s", s_fallback);
        }
    } else if (!strcmp(type, "eyes")) {
        on_eyes(j);
    } else if (!strcmp(type, "push")) {
        /* Only between turns: the voice task plays it like a late reply (timmy_hatch_incoming). */
        if (s_ready && s_phase == P_IDLE && !s_begin_pending && !s_incoming) {
            reset_turn();
            s_push_silent = cJSON_IsTrue(cJSON_GetObjectItem(j, "silent"));
            s_phase = P_REPLY;
            s_incoming = true;
            timmy_state_set_asleep(false);   /* wake the screen for it */
            send_json("{\"type\":\"push_ok\"}");
        } else {
            send_json("{\"type\":\"push_busy\"}");
        }
    } else if (!strcmp(type, "mood")) {
        set_mood(cJSON_GetStringValue(cJSON_GetObjectItem(j, "mood")));
    } else if (!strcmp(type, "error")) {
        fail(text ? text : "TIMMY HAD A PROBLEM");
    } else if (!strcmp(type, "state")) {
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(j, "state"));
        if (st && !strcmp(st, "thinking") && s_phase == P_WAITING) emit(TIMMY_HATCH_EV_SENT, NULL);
    }
    cJSON_Delete(j);
}

/* Reply speech into the speaker ring. Blocks this (the client's) task while
 * the ring is full, which holds the server back by TCP's own flow control. */
static void on_audio(const uint8_t *p, size_t n)
{
    if (s_phase != P_REPLY) return;
    if (s_has_odd && n) {
        uint8_t two[2] = { s_odd, p[0] };
        xStreamBufferSend(s_spk, two, 2, portMAX_DELAY);
        p++, n--;
        s_has_odd = false;
    }
    if (n & 1) {
        s_odd = p[n - 1];
        s_has_odd = true;
        n--;
    }
    while (n && s_phase == P_REPLY && s_ready) {
        size_t sent = xStreamBufferSend(s_spk, p, n, pdMS_TO_TICKS(100));
        p += sent;
        n -= sent;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "socket open, authenticating");
        s_rx_len = 0;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (s_ready) ESP_LOGW(TAG, "connection lost");
        if (s_ready || !s_unready_since) s_unready_since = esp_timer_get_time();
        s_ready = false;
        report(TIMMY_HATCH_UNREACHABLE, id == WEBSOCKET_EVENT_CLOSED ? "Closed (check timmy.key)" : "Can't reach Timmy");
        if (s_phase != P_IDLE) fail("LOST TIMMY - TRY AGAIN");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x2 || (d->op_code == 0x0 && !s_rx_len)) {
            on_audio((const uint8_t *)d->data_ptr, d->data_len);
        } else if (d->op_code == 0x1 || d->op_code == 0x0) {
            if (d->payload_len > RX_MAX) break;
            if (d->payload_offset == 0) s_rx_len = 0;
            if (s_rx_len + d->data_len <= RX_MAX) {
                memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
                s_rx_len += d->data_len;
            }
            if (s_rx_len >= (size_t)d->payload_len) {
                on_text(s_rx, s_rx_len);
                s_rx_len = 0;
            }
        }
        break;
    default:
        break;
    }
}

static void ws_stop(void)
{
    if (!s_ws) return;
    s_ready = false;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

static void ws_start(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = s_away && s_fallback[0] ? s_fallback : s_url,
        .buffer_size = RX_MAX,
        .task_name = "timmy_ws",   /* its stack goes to PSRAM (__wrap_xTaskCreatePinnedToCore) */
        .task_stack = 8192,
        .task_prio = 6,
        .network_timeout_ms = 8000,
        .reconnect_timeout_ms = 3000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 40,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        report(TIMMY_HATCH_UNREACHABLE, "No memory for the socket");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    s_unready_since = esp_timer_get_time();
    if (s_away) ESP_LOGI(TAG, "trying the away address");
    report(TIMMY_HATCH_TESTING, s_away ? "Connecting (away)..." : "Connecting...");
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ws_stop();
        report(TIMMY_HATCH_UNREACHABLE, "Couldn't start the socket");
    }
}

/* Whether the home (LAN) address takes a TCP connection, within a second. */
static bool lan_reachable(void)
{
    char host[64];
    int port = 80;
    const char *p = strstr(s_url, "://");
    if (!p) return false;
    p += 3;
    size_t n = strcspn(p, ":/");
    if (!n || n >= sizeof(host)) return false;
    memcpy(host, p, n);
    host[n] = '\0';
    if (p[n] == ':') port = atoi(p + n + 1);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai) || !ai) return false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = false;
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = { .tv_sec = 1 };
            int err = 0;
            socklen_t len = sizeof(err);
            ok = select(fd + 1, NULL, &w, NULL, &tv) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err;
        }
        close(fd);
    }
    freeaddrinfo(ai);
    return ok;
}

/* Owns the connection and the uplink, so the voice task never waits on the network. */
static void timmy_task(void *arg)
{
    (void)arg;
    static uint8_t piece[UP_PIECE];
    for (;;) {
        bool want = configured() && timmy_wifi_connected();
        if (s_restart || (!want && s_ws)) {
            s_restart = false;
            ws_stop();
        }
        /* Home address not answering (or the fallback not, once away): after a
         * while try the other one, if there is a fallback address. */
        if (s_ws && !s_ready && s_unready_since && s_fallback[0] &&
            esp_timer_get_time() - s_unready_since > SWITCH_AFTER_US) {
            ws_stop();
            s_away = !s_away;
            s_unready_since = 0;
        }
        /* Away and idle: once a minute, see if home is back, and move there. */
        static int64_t next_probe;
        if (s_ws && s_ready && s_away && s_phase == P_IDLE && esp_timer_get_time() >= next_probe) {
            next_probe = esp_timer_get_time() + 60LL * 1000000;
            if (lan_reachable()) {
                ESP_LOGI(TAG, "home address answers again: moving back");
                ws_stop();
                s_away = false;
            }
        }
        static int64_t next_try;
        /* Just woke: a socket that doesn't answer a ping within ALIVE_US is
         * dead; reconnect right away (home first) instead of on the next press. */
        static int64_t alive_sent, alive_checked;
        if (s_check_alive) {
            s_check_alive = false;
            /* Once per wake: the voice loop can leave and re-enter its rest
             * many times while a saved note waits. A connection still being
             * made is left alone; only an idle, established one is checked. */
            int64_t now = esp_timer_get_time();
            if (now - alive_checked > 20LL * 1000000 && !alive_sent) {
                alive_checked = now;
                if (s_ws && s_ready && send_json("{\"type\":\"ping\"}")) {
                    alive_sent = now;
                } else if (want && !s_ws) {
                    next_try = 0;   /* not connected: try now, not after the backoff */
                }
            }
        }
        if (alive_sent && s_pong_at >= alive_sent) {
            ESP_LOGI(TAG, "awake: link alive (%lld ms)", (long long)((s_pong_at - alive_sent) / 1000));
            alive_sent = 0;
        } else if (alive_sent && esp_timer_get_time() - alive_sent > ALIVE_US) {
            ESP_LOGW(TAG, "awake: link didn't answer; reconnecting");
            alive_sent = 0;
            ws_stop();
            s_away = false;
            next_try = 0;
        }
        if (want && !s_ws && esp_timer_get_time() >= next_try) {
            ws_start();
            if (!s_ws) next_try = esp_timer_get_time() + 5000000;   /* no memory or task: back off */
        }
        if (s_cancel_pending) {
            s_cancel_pending = false;
            send_json("{\"type\":\"cancel\"}");
        }
        if (s_begin_pending && s_ready) {
            s_begin_pending = false;
            if (!send_json("{\"type\":\"begin\"}")) fail("CAN'T REACH TIMMY");
        }
        bool sent = false;
        while (!s_begin_pending && s_ready && s_phase == P_TALKING) {
            size_t n = xStreamBufferReceive(s_mic, piece, sizeof(piece), 0);
            if (!n) break;
            if (esp_websocket_client_send_bin(s_ws, (const char *)piece, n, pdMS_TO_TICKS(2000)) < 0) {
                fail("CAN'T KEEP UP - TRY AGAIN");
                break;
            }
            sent = true;
        }
        if (s_end_pending && !s_begin_pending && s_phase == P_TALKING && xStreamBufferIsEmpty(s_mic)) {
            s_end_pending = false;
            if (send_json("{\"type\":\"end\"}")) {
                s_phase = P_WAITING;
                s_t_end = esp_timer_get_time();
            } else {
                fail("CAN'T REACH TIMMY");
            }
        }
        if (s_phase == P_WAITING && esp_timer_get_time() - s_t_end > REPLY_TIMEOUT_US) fail("NO REPLY FROM TIMMY");
        if (s_phase == P_TALKING && s_end_pending && !s_ready) fail("CAN'T REACH TIMMY");
        if (!sent) vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*
 * esp_websocket_client makes its task with xTaskCreate, whose stack comes from
 * internal RAM, and this board has only a few KB of that left once Wi-Fi, BLE
 * and the display are up. The link wraps xTaskCreatePinnedToCore (CMakeLists.txt)
 * so that one task alone gets its stack in PSRAM; it only does network I/O. A
 * stopped client's task deletes itself, which leaves its PSRAM stack behind:
 * 6 KB per reconnect after a settings change, rare enough.
 */
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, const configSTACK_DEPTH_TYPE depth,
                                          void *arg, UBaseType_t prio, TaskHandle_t *handle, const BaseType_t core);
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, const configSTACK_DEPTH_TYPE depth,
                                          void *arg, UBaseType_t prio, TaskHandle_t *handle, const BaseType_t core)
{
    if (name && !strcmp(name, "timmy_ws")) {
        return xTaskCreatePinnedToCoreWithCaps(fn, name, depth, arg, prio, handle, core,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return __real_xTaskCreatePinnedToCore(fn, name, depth, arg, prio, handle, core);
}

/* ---- Public: the voice task's side ---- */

void timmy_hatch_start(void)
{
    if (nvs_open("timmy", NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "no NVS");
        return;
    }
    load_settings();
    s_events = xQueueCreate(12, sizeof(ev_t));
    s_lock = xSemaphoreCreateMutex();
    static StaticStreamBuffer_t mic_sb, spk_sb;
    uint8_t *mic = heap_caps_malloc(MIC_RING + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *spk = heap_caps_malloc(SPK_RING + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rx = heap_caps_malloc(RX_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mic || !spk || !s_rx || !s_events || !s_lock) {
        ESP_LOGE(TAG, "no memory for Timmy's buffers");
        return;
    }
    s_mic = xStreamBufferCreateStatic(MIC_RING, 1, mic, &mic_sb);
    s_spk = xStreamBufferCreateStatic(SPK_RING, 1, spk, &spk_sb);
    /* PSRAM stack: it never touches flash (NVS writes happen on the console's task). */
    if (xTaskCreatePinnedToCoreWithCaps(timmy_task, "timmy", 4096, NULL, 5, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no task");
        return;
    }
    ESP_LOGI(TAG, "Timmy mode: %s as %s, key %s", s_url[0] ? s_url : "(no url)", s_dev, s_have_key ? "set" : "not set");
}

bool timmy_hatch_ready(void)
{
    return s_mic && configured() && timmy_wifi_connected() && s_ready;
}

static void reset_turn(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_text[0] = '\0';
    s_text_len = 0;
    s_n_say = 0;
    s_frames_total = s_say_left = 0;
    s_has_odd = false;
    xSemaphoreGive(s_lock);
    xQueueReset(s_events);
    s_got_done = false;
}

void timmy_hatch_turn_begin(void)
{
    if (!s_mic) return;
    s_mood = TIMMY_MOOD_NONE;   /* the next reply brings its own */
    s_incoming = false;        /* a press wins over something Timmy was about to say */
    if (s_phase != P_IDLE) s_cancel_pending = true;
    s_phase = P_IDLE;   /* stops on_audio before the rings reset */
    vTaskDelay(1);
    xStreamBufferReset(s_mic);
    xStreamBufferReset(s_spk);
    reset_turn();
    s_end_pending = false;
    s_phase = P_TALKING;
    s_begin_pending = true;
}

void timmy_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    if (s_phase != P_TALKING) return;
    if (xStreamBufferSend(s_mic, pcm, frames * 2, 0) != frames * 2) fail("MIC BACKLOG FULL - TRY AGAIN");
}

size_t timmy_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    if (s_phase != P_TALKING) return frames;   /* failed: let the caller move on */
    size_t room = xStreamBufferSpacesAvailable(s_mic) / 2;
    if (!room && wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        room = xStreamBufferSpacesAvailable(s_mic) / 2;
    }
    size_t take = frames < room ? frames : room;
    return xStreamBufferSend(s_mic, pcm, take * 2, 0) / 2;
}

void timmy_hatch_turn_end(void)
{
    if (s_phase != P_TALKING) {
        emit(TIMMY_HATCH_EV_ERROR, "CAN'T REACH TIMMY");
        return;
    }
    s_end_pending = true;
}

void timmy_hatch_turn_cancel(void)
{
    if (s_phase != P_IDLE) s_cancel_pending = true;
    s_phase = P_IDLE;
    s_end_pending = s_begin_pending = false;
    vTaskDelay(1);
    xStreamBufferReset(s_spk);
    xQueueReset(s_events);
}

bool timmy_hatch_incoming(void)
{
    if (!s_incoming) return false;
    s_incoming = false;
    return true;
}

bool timmy_timmy_push_silent(void)
{
    return s_push_silent;
}

timmy_hatch_ev_t timmy_hatch_turn_event(char *text, size_t cap)
{
    ev_t ev;
    if (!s_events || xQueueReceive(s_events, &ev, 0) != pdTRUE) return TIMMY_HATCH_EV_NONE;
    strlcpy(text, ev.text, cap);
    return ev.type;
}

size_t timmy_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    if (!s_spk) return 0;
    size_t n = xStreamBufferReceive(s_spk, pcm, frames * 2, pdMS_TO_TICKS(wait_ms > 0 ? wait_ms : 0));
    if (n & 1) n += xStreamBufferReceive(s_spk, (uint8_t *)pcm + n, 1, pdMS_TO_TICKS(20));
    return n / 2;
}

bool timmy_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_n_say && s_text_len) {
        int i = 0;
        while (i + 1 < s_n_say && played >= s_say[i + 1].start) i++;
        const say_t *s = &s_say[i];
        size_t into = played > s->start ? played - s->start : 0;
        size_t at = s->off + (size_t)((uint64_t)s->len * (into < s->frames ? into : s->frames) / s->frames);
        if (at >= s_text_len) at = s_text_len - 1;
        ok = timmy_hatch_caption_at(s_text, at, out, cap);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

float timmy_turn_progress(size_t played, int *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t total = s_frames_total;
    int said = s_n_say;
    xSemaphoreGive(s_lock);
    if (message) *message = total ? 0 : -1;
    if (!total) return 0;
    /* Sentences arrive one at a time, so until "done" the length is a guess:
     * what has come so far plus about one more sentence of it on average.
     * timmy_voice.c keeps the ring moving forward only. */
    float expect = s_got_done ? (float)total : (float)total + (float)total / said;
    float p = (float)played / expect;
    return p > 1 ? 1 : p;
}

size_t timmy_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}

void timmy_hatch_text_turn(char *text)
{
    free(text);
    timmy_hatch_console("error", "typed turns aren't supported in Timmy mode", NULL);
}

void timmy_hatch_text_cancel(void)
{
}
