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

#include "timmy_voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "timmy_adpcm.h"
#include "timmy_audio.h"
#include "timmy_board.h"
#include "timmy_chat.h"
#include "timmy_input.h"
#include "timmy_mem.h"
#include "timmy_settings.h"
#include "timmy_state.h"
#include "timmy_wifi.h"
#define ASSISTANT "TIMMY"

/* Progress captions (recording, sending, saved notes). Timmy's eyes show
 * these states, so its build leaves the caption empty and keeps only what was
 * heard, the reply, and errors. */
#define SAVED_NOTICE "SAVED - I'LL ANSWER WHEN I'M BACK"
#define FULL_NOTICE "TOO MANY WAITING - TRY AGAIN WHEN I'M BACK"
#define OLD_ANSWER_US (30LL * 60 * 1000000)   /* older questions get a silent answer */
static bool s_reply_silent;     /* this reply: captions only, no sound */
static volatile bool s_notice;  /* a silent answer on screen until a tap */
#define status_caption(...) timmy_state_set_caption("%s", "")
#define STATUS(text) ""

static const char *TAG = "timmy_voice";

#define MAX_SECS 15
#define TAIL_FRAMES (TIMMY_AUDIO_RATE * 12 / 100)   /* capture lag + poll interval, stops before the release click */
#define MAX_FRAMES (TIMMY_AUDIO_RATE * MAX_SECS)
#define NOTE_MIN_HOLD_US 600000
#define CALL_TAP_US 300000   /* tap-to-call keeps its tap length, as a waking PTT tap */
#define CHUNK_US (TIMMY_AUDIO_CHUNK * 1000000 / TIMMY_AUDIO_RATE)
#define NOTE_MIN_RECORD_FRAMES (TIMMY_AUDIO_RATE / 2)   /* duration, not detected speech */
#define NOTE_TOGGLE 0
#define PRE_CHUNKS 16                                  /* 320 ms of audio kept from before the press */
#define PRE_CAP_CHUNKS (PRE_CHUNKS + NOTE_MIN_HOLD_US / CHUNK_US)
#define SETTLE_CHUNKS 10   /* after Timmy makes a sound, 200 ms of capture is its own tail */
#define REST_BACKSTOP_MS 60000

/* A note recorded while Hatch is out of reach is saved in PSRAM, and goes once it's back. */
#define HOLD_NOTES 1
#define HELD_MAX 4
#define HELD_TRIES 8                            /* then a saved note is dropped */
#define HELD_POLL_MS 5000                       /* resting with notes saved: check for Wi-Fi this often */
#define HELD_KEEP_US (30LL * 60 * 1000000)      /* timmy_voice_notes_waiting() */
#define RETRY_MIN_US (15LL * 1000000)
#define RETRY_MAX_US (120LL * 1000000)
#define STALL_US (30LL * 1000000)               /* Hatch took none of a note this long: give up */
#define ACK_WAIT_US (30LL * 1000000)            /* asleep, waiting for the VM to have a note */
#define REPLY_COMPLETE_HOLD_MS 300
#define REPLY_COMPLETE_POLL_MS 10

static QueueHandle_t s_queue;
static uint32_t s_press_input_epoch;
static uint32_t s_press_mark;   /* the press's pending-call mark (timmy_input_clear_call_pending) */
static int64_t s_press_us;
static volatile bool s_monitor;
static volatile float s_monitor_db = -100.0f;
static volatile bool s_chirp;
static volatile bool s_loopback;
static volatile bool s_mp3test;
static atomic_int s_call_view;
static atomic_bool s_ptt_call;
static atomic_bool s_shutdown;

timmy_voice_view_t timmy_voice_call_view(void) { return atomic_load(&s_call_view); }
bool timmy_voice_in_call(void) { return timmy_voice_call_view() != TIMMY_VOICE_VIEW_NONE; }
bool timmy_voice_ptt_call(void) { return timmy_voice_in_call() && atomic_load(&s_ptt_call); }
bool timmy_voice_ptt_enabled(void)
{
    return false;
}

void timmy_voice_shutdown(int wait_ms)
{
    /* Input-task callers publish only this signal. The voice task owns the
     * core's capture state and calls end from call_input(), never here. */
    atomic_store(&s_shutdown, true);
    int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (timmy_voice_in_call() && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void timmy_voice_cancel_shutdown(void) { atomic_store(&s_shutdown, false); }

const char *timmy_voice_call_label(void)
{
    if (!timmy_voice_ptt_call()) return NULL;
    switch (timmy_voice_call_view()) {
    case TIMMY_VOICE_VIEW_CONNECTING: return "CONNECTING";
    case TIMMY_VOICE_VIEW_READY: return "HOLD TO TALK";
    case TIMMY_VOICE_VIEW_LISTENING: return "LISTENING";
    case TIMMY_VOICE_VIEW_THINKING: return "THINKING";
    case TIMMY_VOICE_VIEW_WORKING: return "WORKING";
    case TIMMY_VOICE_VIEW_SPEAKING: return "SPEAKING";
    case TIMMY_VOICE_VIEW_ENDING: return "ENDING CALL";
    default: return NULL;
    }
}

const char *timmy_voice_call_hint(void)
{
    if (!timmy_voice_ptt_call()) return NULL;
    switch (timmy_voice_call_view()) {
    case TIMMY_VOICE_VIEW_LISTENING: return "RELEASE";
    case TIMMY_VOICE_VIEW_THINKING:
    case TIMMY_VOICE_VIEW_SPEAKING: return "HOLD TO\nINTERRUPT";
    case TIMMY_VOICE_VIEW_ENDING: return "";
    default: return "HOLD TO\nTALK";
    }
}

/*
 * Pre-roll: while idle the mic keeps running into this ring, so a recording
 * can start a little before the press. People start talking as they press,
 * and the power key reports the press late.
 */
typedef struct {
#if TIMMY_LOW_MEM
    timmy_adpcm_t start;   /* encoder state before this chunk, to decode it alone */
    uint8_t adpcm[TIMMY_AUDIO_CHUNK / 2];
#else
    int16_t pcm[TIMMY_AUDIO_CHUNK];
#endif
} pre_chunk_t;

static pre_chunk_t *s_pre;   /* PRE_CAP_CHUNKS: idle pre-roll plus the unsent hold */
static size_t s_pre_next, s_pre_fill;
#if TIMMY_LOW_MEM
static timmy_adpcm_t s_pre_enc;
#endif
static int16_t s_chunk[TIMMY_AUDIO_CHUNK];
static int s_settle;

/*
 * The note being recorded (HOLD_NOTES), kept whole in case it can't go now.
 * Streaming, Hatch has had s_sent frames of it.
 */
static int16_t *s_rec;   /* MAX_FRAMES */
static size_t s_rec_n, s_sent;
static bool s_live = true;
static bool s_tried;     /* it went to Hatch */

typedef struct {
    int16_t *pcm;
    size_t frames;
    int64_t at_us;   /* when it was recorded */
    int tries;
} held_note_t;

static int s_held_count;
static volatile bool s_waiting;        /* timmy_voice_notes_waiting() */
#if HOLD_NOTES
static held_note_t s_held[HELD_MAX];   /* oldest first */
static int64_t s_next_send_us;
static int64_t s_send_backoff_us = RETRY_MIN_US;
#endif

/* The i-th oldest pre-roll chunk, into out. */
static void pre_get(size_t i, int16_t *out)
{
    const pre_chunk_t *c = &s_pre[(s_pre_next + PRE_CAP_CHUNKS - s_pre_fill + i) % PRE_CAP_CHUNKS];
#if TIMMY_LOW_MEM
    timmy_adpcm_t dec = c->start;
    timmy_adpcm_decode_block(&dec, c->adpcm, TIMMY_AUDIO_CHUNK, out);
#else
    memcpy(out, c->pcm, sizeof(c->pcm));
#endif
}

/* Call after Timmy plays anything: its own sound must not become pre-roll. */
static void pre_reset(void)
{
    s_pre_fill = 0;
    s_settle = SETTLE_CHUNKS;
}

/* Stores one successful read in the pre-roll (or the isolated waking hold). */
static void pre_put(void)
{
    pre_chunk_t *c = &s_pre[s_pre_next];
#if TIMMY_LOW_MEM
    c->start = s_pre_enc;
    timmy_adpcm_encode_block(&s_pre_enc, s_chunk, TIMMY_AUDIO_CHUNK, c->adpcm);
#else
    memcpy(c->pcm, s_chunk, sizeof(c->pcm));
#endif
    s_pre_next = (s_pre_next + 1) % PRE_CAP_CHUNKS;
    s_pre_fill = s_pre_fill < PRE_CAP_CHUNKS ? s_pre_fill + 1 : PRE_CAP_CHUNKS;
}

/* Outside a note's hold, the ring keeps only PRE_CHUNKS of pre-roll. */
static void pre_put_preroll(void)
{
    pre_put();
    if (s_pre_fill > PRE_CHUNKS) s_pre_fill = PRE_CHUNKS;
}

/* One 20 ms idle read: feeds the pre-roll ring and the settings mic meter. */
static void idle_capture(void)
{
    if (timmy_audio_read(s_chunk, TIMMY_AUDIO_CHUNK) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (s_monitor) {
        float db = timmy_audio_dbfs(s_chunk, TIMMY_AUDIO_CHUNK);
        s_monitor_db = db > s_monitor_db ? db : s_monitor_db * 0.9f + db * 0.1f;
    }
    if (s_settle > 0) {
        s_settle--;
        return;
    }
    pre_put_preroll();
}

/* Consume through the requested edge, leaving later edges for the next
 * phase (notably a quick release after interrupting a saved note's reply). */
static bool got_event(timmy_ptt_t type)
{
    timmy_input_event_t ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        timmy_state_poke();
        if (ev.epoch == timmy_input_call_epoch() && ev.type == type) {
            if (type == TIMMY_PTT_DOWN) {
                s_press_input_epoch = ev.epoch;
                s_press_mark = ev.mark;
                s_press_us = ev.at_us;
            }
            return true;
        }
        timmy_input_clear_call_pending(ev.mark);   /* a press dropped here starts nothing */
    }
    return false;
}

/* Level stats of a recording, gathered chunk by chunk for the log. */
typedef struct {
    double acc;
    size_t frames;
    int peak, clipped;
    float floor_db;
    float tail_db[25];   /* the last 0.5 s, to see where speech ends and the release click lands */
    size_t chunks;
} rec_stats_t;

/* Adds one captured chunk (up to TIMMY_AUDIO_CHUNK frames) to the stats. */
static void keep(rec_stats_t *st, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = abs(pcm[i]);
        st->peak = v > st->peak ? v : st->peak;
        st->clipped += v >= 32000;
        st->acc += (double)pcm[i] * pcm[i];
    }
    st->frames += n;
    if (n == TIMMY_AUDIO_CHUNK) {
        float db = timmy_audio_dbfs(pcm, n);
        st->floor_db = db < st->floor_db ? db : st->floor_db;
        st->tail_db[st->chunks++ % 25] = db;
    }
}

/* Gives Hatch what it has room for of the note being recorded. */
static void feed_live(void)
{
#if HOLD_NOTES
    if (s_live && s_sent < s_rec_n) {
        s_sent += timmy_hatch_turn_audio_wait(s_rec + s_sent, s_rec_n - s_sent, 0);
    }
#endif
}

/* Streams the note being recorded from its start, then the rest as it comes. */
static void go_live(void)
{
    timmy_hatch_turn_begin();
    s_live = s_tried = true;
    s_sent = 0;
    feed_live();
}

/* One chunk of speech: into the stats, and on to Hatch, by way of the kept note if there is one. */
static void take(rec_stats_t *st, const int16_t *pcm)
{
    keep(st, pcm, TIMMY_AUDIO_CHUNK);
    if (!s_rec) {
        if (s_live) {
            timmy_hatch_turn_audio(pcm, TIMMY_AUDIO_CHUNK);
        }
        return;
    }
    memcpy(s_rec + s_rec_n, pcm, TIMMY_AUDIO_CHUNK * sizeof(int16_t));
    s_rec_n += TIMMY_AUDIO_CHUNK;
    feed_live();
}

/* Consume buffered audio in order, freeing ring space for new mic reads. */
static void take_pre(rec_stats_t *st, size_t chunks)
{
    while (chunks-- && s_pre_fill) {
        pre_get(0, s_chunk);
        s_pre_fill--;
        take(st, s_chunk);
    }
}

/*
 * Records an admitted note until release (the next press in toggle mode) or
 * MAX_SECS, starting with the buffered pre-roll and hold. It streams to Timmy
 * as it goes if it can.
 * If not, the note is kept in s_rec to send later, and streams from partway
 * if Hatch comes within reach. Returns false if Hatch failed the turn with no
 * kept note to fall back on (why says what failed). There is no start chirp:
 * anything played now would land on top of the first words.
 */
static bool record(char *why, size_t cap)
{
    s_notice = false;
    timmy_state_set_mode(TIMMY_MODE_LISTENING);
    timmy_state_set_progress(0);
    s_rec_n = s_sent = 0;
    s_live = s_tried = false;
    if (!s_rec || (timmy_hatch_ready() && !s_held_count)) {
        go_live();
    }
    status_caption(s_live ? "LISTENING..." : "RECORDING...");
    bool heard = false, ok = true;
    bool gave_up = false;   /* Hatch failed this note: it's kept, and goes later */
    char text[96];
    rec_stats_t st = { 0 };

    size_t n = s_pre_fill * TIMMY_AUDIO_CHUNK;
    /* Link-only boards upload synchronously. Replay the longer buffered hold
     * in short bursts between mic reads, rather than stalling capture while
     * sending the whole prefix. The voice-session backend has a larger FIFO. */
    take_pre(&st, TIMMY_LOW_MEM ? PRE_CHUNKS : s_pre_fill);
    bool released = false;
    size_t stop_at = MAX_FRAMES;
    while (n + TIMMY_AUDIO_CHUNK <= stop_at) {
        if (timmy_audio_read(s_chunk, TIMMY_AUDIO_CHUNK) != ESP_OK) {
            break;
        }
        timmy_state_set_level(timmy_audio_level(s_chunk, TIMMY_AUDIO_CHUNK));
#if TIMMY_LOW_MEM
        if (s_pre_fill) {
            /* Keep new input behind the opening backlog until it drains. */
            pre_put();
            take_pre(&st, PRE_CHUNKS);
        } else
#endif
        {
            take(&st, s_chunk);   /* fresh PCM needs no lossy buffer round-trip */
        }
        /* Live transcript as the caption. A failure stops the streaming; the
         * kept note goes later, or without one the failure is the caption. */
        timmy_hatch_ev_t ev;
        while (s_live && (ev = timmy_hatch_turn_event(text, sizeof(text))) != TIMMY_HATCH_EV_NONE) {
            if (ev == TIMMY_HATCH_EV_HEARD && text[0]) {
                heard = true;
                timmy_state_set_caption("%s", text);
            } else if (ev == TIMMY_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "timmy: %s", text);
                timmy_hatch_turn_cancel();
                s_live = false;
                gave_up = true;
                if (!s_rec) {
                    ok = false;
                    strlcpy(why, text, cap);
                    timmy_state_set_caption("%s", text);
                }
            }
        }
        n += TIMMY_AUDIO_CHUNK;
        bool tick = n % (TIMMY_AUDIO_CHUNK * 5) == 0;
        if (tick && s_rec && !s_live && !gave_up && !s_held_count && timmy_hatch_ready()) {
            ESP_LOGI(TAG, "Timmy in reach: streaming the note so far");
            go_live();
        }
        timmy_state_set_progress((float)n / MAX_FRAMES);
        if (!heard && ok && tick) {
            status_caption(s_live ? "LISTENING %.1fs" : "RECORDING %.1fs", (double)n / TIMMY_AUDIO_RATE);
        }
        /*
         * Capture runs 60-80 ms behind real time and people let go on their
         * last syllable, so keep going briefly after release.
         */
        if (!released && got_event(NOTE_TOGGLE ? TIMMY_PTT_DOWN : TIMMY_PTT_UP)) {
            released = true;
            stop_at = n + TAIL_FRAMES < MAX_FRAMES ? n + TAIL_FRAMES : MAX_FRAMES;
        }
    }
    take_pre(&st, s_pre_fill);
    timmy_state_set_level(0);

    char tail[160];
    int tl = 0;
    size_t shown = st.chunks < 25 ? st.chunks : 25;
    for (size_t i = st.chunks - shown; i < st.chunks; i++) {
        tl += snprintf(tail + tl, sizeof(tail) - tl, " %.0f", st.tail_db[i % 25]);
    }
    ESP_LOGI(TAG, "end levels:%s", tail);
    float rms_db = st.frames ? 10.0f * log10f((float)(st.acc / st.frames / (32768.0 * 32768.0)) + 1e-10f) : -100.0f;
    ESP_LOGI(TAG, "recorded %.2fs%s: rms %.1f dBFS, peak %.1f dBFS, floor %.1f dBFS, %d clipped, gain %d dB",
             (double)n / TIMMY_AUDIO_RATE, released ? "" : " (max)", rms_db < -100.0f ? -100.0f : rms_db,
             20.0 * log10((st.peak + 1) / 32768.0), st.floor_db, st.clipped, timmy_settings_mic_gain());
    return ok;
}

static void go_idle(const char *caption);

/*
 * Plays Hatch's reply as it arrives, with its text as the caption. Returns
 * true if interrupted by a new press. *delivered: the VM has the note.
 * incoming: a reply to an earlier request, which nobody pressed for.
 */
static bool hatch_reply(bool *delivered, bool incoming)
{
    timmy_state_set_progress(0); /* Recording progress must not carry into the reply. */
    timmy_state_set_mode(TIMMY_MODE_THINKING);
    /* Until there's a transcript or reply; a late reply has no note to send. */
    status_caption(incoming ? ASSISTANT " HAS AN ANSWER" : "SENDING VOICE NOTE");
    static int16_t buf[TIMMY_AUDIO_CHUNK];
    static const int16_t silence[TIMMY_AUDIO_CHUNK];
    char text[96];
    static char page[TIMMY_CAPTION_MAX];
    char incomplete[sizeof(text)] = "";
    bool done = false, speaking = false, replied = false;
    size_t played = 0;
    int progress_message = -1;
    int progress_logged = -1;
    float progress = 0;
    int64_t t0 = esp_timer_get_time();
    *delivered = false;
    for (;;) {
        timmy_state_poke();   /* an active turn is activity: don't auto-sleep mid-reply */
        timmy_hatch_ev_t ev;
        while ((ev = timmy_hatch_turn_event(text, sizeof(text))) != TIMMY_HATCH_EV_NONE) {
            switch (ev) {
            case TIMMY_HATCH_EV_HEARD:
                if (!speaking && !replied) {
                    timmy_state_set_caption("\"%s\"", text);
                }
                break;
            case TIMMY_HATCH_EV_SENT:
                *delivered = true;
                if (!speaking && !replied) {
                    status_caption("NOTE SENT - WAITING FOR " ASSISTANT);
                }
                break;
            case TIMMY_HATCH_EV_REPLY:
                replied = *delivered = true;
                /* Once speech starts, the caption follows it. The event only
                 * has room for the page's start; the page itself comes below. */
                if (!speaking && !timmy_hatch_turn_caption(played, page, sizeof(page))) {
                    timmy_state_set_caption("%s", text);
                }
                break;
            case TIMMY_HATCH_EV_DONE:
                done = *delivered = true;
                strlcpy(incomplete, text, sizeof(incomplete));
                break;
            case TIMMY_HATCH_EV_ERROR:
                ESP_LOGW(TAG, "timmy: %s", text);
                timmy_state_set_level(0);
                go_idle(text);
                return false;
            default:
                break;
            }
        }
        if (got_event(TIMMY_PTT_DOWN)) {
            ESP_LOGI(TAG, "reply interrupted");
            timmy_hatch_turn_cancel();
            timmy_state_set_progress(0);
            timmy_state_set_level(0);
            return true;
        }
        size_t n = timmy_hatch_turn_read(buf, TIMMY_AUDIO_CHUNK, speaking || done ? 0 : 20);
        if (n) {
            if (!speaking) {
                speaking = true;
                timmy_state_set_mode(TIMMY_MODE_SPEAKING);
                ESP_LOGI(TAG, "reply audio after %.2fs", (esp_timer_get_time() - t0) / 1e6);
            }
            timmy_state_set_level(timmy_audio_level(buf, n));
            static const int16_t hush[TIMMY_AUDIO_CHUNK];
            timmy_audio_write(s_reply_silent ? hush : buf, n);   /* silent: same pace, no sound */
            played += n;
        } else if (done) {
            timmy_state_set_progress(!incomplete[0] && played ? 1 : 0);
            ESP_LOGI(TAG, "reply progress: %s", incomplete[0] ? "incomplete" : "complete");
            break;
        } else if (speaking) {
            /* Between messages: keep the speaker fed so it doesn't replay stale DMA. */
            timmy_state_set_level(0);
            timmy_audio_write(silence, TIMMY_AUDIO_CHUNK);
        }
        if (speaking) {
            int message;
            float next = timmy_turn_progress(played, &message);
            if (message != progress_message) {
                progress_message = message;
                progress_logged = -1;
            }
            /* Keep the ring forward-only across messages. Their count is unknown,
             * so hold at 99% until the entire turn has completed successfully. */
            if (next > 0.99f) next = 0.99f;
            if (next > progress) progress = next;
            timmy_state_set_progress(incomplete[0] ? 0 : progress);
            int percent = (int)(progress * 100);
            int bucket = percent == 99 ? 99 : percent / 10 * 10;
            if (!incomplete[0] && message >= 0 && bucket != progress_logged) {
                progress_logged = bucket;
                ESP_LOGI(TAG, "reply progress: message %d, %d%% (PCM cursor %.2fs)",
                         message, percent, (double)played / TIMMY_AUDIO_RATE);
            }
        }
        /* The page being said, or before the speech the reply's opening page. */
        if ((speaking || replied) && timmy_hatch_turn_caption(played, page, sizeof(page))) {
            timmy_state_set_caption("%s", page);
        }
    }
    timmy_state_set_level(0);
    ESP_LOGI(TAG, "timmy reply: %.2fs of audio, %.2fs total", (double)played / TIMMY_AUDIO_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (incomplete[0]) {
        ESP_LOGW(TAG, "timmy: %s", incomplete);
        go_idle(incomplete);
        return false;
    }
    if (!played) {
        /* No speech (TTS unavailable): leave the reply text up for a moment. */
        vTaskDelay(pdMS_TO_TICKS(2500));
    } else {
        /* Keep the final caption and full ring visible long enough to register.
         * A new press still interrupts immediately and starts the next note. */
        for (int elapsed = 0; elapsed < REPLY_COMPLETE_HOLD_MS;
             elapsed += REPLY_COMPLETE_POLL_MS) {
            if (got_event(TIMMY_PTT_DOWN)) {
                ESP_LOGI(TAG, "reply interrupted after completion");
                timmy_hatch_turn_cancel();
                timmy_state_set_progress(0);
                timmy_state_set_level(0);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(REPLY_COMPLETE_POLL_MS));
        }
    }
    return false;
}

static void go_idle(const char *caption)
{
    timmy_state_poke();   /* the reading clock starts when the turn ends, not at the press */
    timmy_state_set_progress(0);
    timmy_state_set_mode(TIMMY_MODE_IDLE);
    timmy_state_set_caption("%s", caption);
}

/* Errors and status stay up; a press too short to count only needs a hint. */
static void too_short(void)
{
    timmy_state_set_progress(0);
    timmy_state_set_mode(TIMMY_MODE_IDLE);
#if NOTE_TOGGLE
    timmy_state_set_hint("RECORD A LONGER NOTE");
#else
    timmy_state_set_hint("HOLD LONGER TO TALK");
#endif
}

/* Why a press can't go to Hatch; voice notes only go there. */
static const char *not_ready_reason(void)
{
    timmy_hatch_status_t st;
    timmy_hatch_status(&st);
    switch (st.state) {
    case TIMMY_HATCH_NOT_SET: return "SET UP " ASSISTANT " FIRST";
    case TIMMY_HATCH_OFFLINE: return "NO WI-FI";
    default: return "CAN'T REACH " ASSISTANT;
    }
}

static volatile bool s_resting;

/*
 * Asleep on battery with nothing to do: power the codecs down (the loop rests
 * Wi-Fi alongside). They come back within one wait on waking, and a talk press
 * that woke Timmy is kept for the loop (a note may follow). On USB power
 * nothing changes asleep.
 */
static void set_resting(bool rest)
{
    if (rest == s_resting) {
        return;
    }
    s_resting = rest;
    timmy_audio_power(!rest);
    timmy_hatch_set_resting(rest);
    if (rest) {
        /* Presses from before sleeping are stale, but not one that just woke Timmy. */
        timmy_input_event_t ev;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
            if (ev.wake) {
                xQueueSendToFront(s_queue, &ev, 0);
                break;
            }
            timmy_input_clear_call_pending(ev.mark);   /* this press won't start a call */
        }
    } else {
        pre_reset();
    }
    ESP_LOGI(TAG, "%s", rest ? "resting: codecs off, Wi-Fi modem sleep" : "awake: codecs on");
}

/* The Watcher's input layer already distinguishes wheel clicks from holds.
 * Preserve its existing note gesture rather than adding a second hold gate. */
static bool note_requires_hold(void)
{
    return true;
}

typedef enum {
    NOTE_ACCEPTED,
    NOTE_TAP,          /* released within CALL_TAP_US: may start a call */
    NOTE_SHORT_HOLD,   /* released after a tap but before NOTE_MIN_HOLD_US */
    NOTE_INSUFFICIENT_CAPTURE,
    NOTE_CAPTURE_ERROR,
    NOTE_CANCELLED,
} note_result_t;

/*
 * Buffer locally until this press has lasted 600 ms AND supplied 500 ms of
 * successful capture while held (toggle mode: 500 ms of capture before the
 * press that stops it; letting go of the first press doesn't count). Idle
 * pre-roll, playback settling and release tail never count. No turn is
 * opened until NOTE_ACCEPTED; the ring keeps the first words on boards both
 * with and without a whole-note buffer.
 * A waking press changes nothing on screen until it qualifies: a tap only wakes.
 */
static note_result_t note_held(bool wake, bool barge_in)
{
    if (wake || barge_in) s_pre_fill = 0;
    int settle = barge_in ? SETTLE_CHUNKS : 0;
    size_t captured = 0;
    if (!wake) {
        timmy_state_set_mode(TIMMY_MODE_LISTENING);
        status_caption("RECORDING...");
    }
    for (;;) {
        if (atomic_load(&s_shutdown) || s_press_input_epoch != timmy_input_call_epoch()) return NOTE_CANCELLED;
        timmy_input_event_t ev;
        bool edge = xQueuePeek(s_queue, &ev, 0) == pdTRUE;
        int64_t until = edge ? ev.at_us : esp_timer_get_time();
        bool eligible = !settle && (NOTE_TOGGLE ? captured >= NOTE_MIN_RECORD_FRAMES :
            !note_requires_hold() ||
            (until - s_press_us >= NOTE_MIN_HOLD_US && captured >= NOTE_MIN_RECORD_FRAMES));
        if (edge && NOTE_TOGGLE && ev.type != TIMMY_PTT_END_CALL) {
            if (ev.type == TIMMY_PTT_UP) {
                xQueueReceive(s_queue, &ev, 0);   /* letting go doesn't stop a toggled note */
                continue;
            }
            /* The press that stops it: record() takes it, or it was too soon. */
            if (eligible) return NOTE_ACCEPTED;
            xQueueReceive(s_queue, &ev, 0);
            return NOTE_INSUFFICIENT_CAPTURE;
        }
        if (edge) {
            /* Leave an admitted release for record()'s capture-lag tail, and
             * a later DOWN for the next gesture. Never borrow its hold time. */
            if (ev.epoch == s_press_input_epoch && ev.type == TIMMY_PTT_UP) {
                if (eligible) return NOTE_ACCEPTED;
                xQueueReceive(s_queue, &ev, 0);
                int64_t held_us = until - s_press_us;
                return held_us < CALL_TAP_US ? NOTE_TAP :
                    held_us < NOTE_MIN_HOLD_US ? NOTE_SHORT_HOLD : NOTE_INSUFFICIENT_CAPTURE;
            }
            if (ev.type == TIMMY_PTT_DOWN) return NOTE_CANCELLED;
            xQueueReceive(s_queue, &ev, 0);
            timmy_input_clear_call_pending(ev.mark);
            if (ev.type == TIMMY_PTT_END_CALL) return NOTE_CANCELLED;
            continue;
        }
        if (eligible) return NOTE_ACCEPTED;
        if (timmy_audio_read(s_chunk, TIMMY_AUDIO_CHUNK) != ESP_OK) {
            return atomic_load(&s_shutdown) || s_press_input_epoch != timmy_input_call_epoch() ?
                NOTE_CANCELLED : NOTE_CAPTURE_ERROR;
        }
        if (settle) {
            settle--;
            continue;
        }
        pre_put();
        if (!wake) timmy_state_set_level(timmy_audio_level(s_chunk, TIMMY_AUDIO_CHUNK));
        /* Conservatively exclude a block whose read overlapped release. */
        if (xQueuePeek(s_queue, &ev, 0) != pdTRUE) captured += TIMMY_AUDIO_CHUNK;
    }
}

/* A waking PTT tap only wakes. Start a new, held-only ring at DOWN: none of
 * the idle pre-roll belongs to this press. UP leaves a later DOWN queued. */
static bool ptt_held_on_waking(void)
{
    s_pre_fill = 0;
    s_settle = 0;
    int64_t until = esp_timer_get_time() + 300000;
    for (;;) {
        timmy_input_event_t ev;
        if (s_press_input_epoch != timmy_input_call_epoch()) {
            s_pre_fill = 0;
            return false;
        }
        if (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
            s_pre_fill = 0;
            if (ev.type != TIMMY_PTT_DOWN) xQueueReceive(s_queue, &ev, 0);
            return false;
        }
        if (esp_timer_get_time() >= until) return true;
        if (timmy_audio_read(s_chunk, TIMMY_AUDIO_CHUNK) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (xQueuePeek(s_queue, &ev, 0) != pdTRUE) {
            pre_put_preroll();   /* still held after the blocking read */
        }
    }
}

static void drop_rec(void)
{
    free(s_rec);
    s_rec = NULL;
}

#if HOLD_NOTES
/* True if a press is queued, left there for the loop; releases ahead of it are dropped. */
static bool press_waiting(void)
{
    timmy_input_event_t ev;
    while (xQueuePeek(s_queue, &ev, 0) == pdTRUE) {
        if (ev.type == TIMMY_PTT_DOWN) {
            return true;
        }
        xQueueReceive(s_queue, &ev, 0);
    }
    return false;
}

static void back_off(void)
{
    s_next_send_us = esp_timer_get_time() + s_send_backoff_us;
    s_send_backoff_us = s_send_backoff_us * 2 > RETRY_MAX_US ? RETRY_MAX_US : s_send_backoff_us * 2;
}

/* Saves the note just recorded to send later. `tried`: Hatch failed it, so it waits out the backoff. */
static void hold_rec(bool tried)
{
    if (s_held_count >= HELD_MAX) {   /* can_record() leaves room: not expected */
        drop_rec();
        go_idle("COULDN'T SAVE THE NOTE");
        return;
    }
    if (!s_rec_n) {   /* nothing captured: realloc to 0 would free s_rec and return NULL */
        drop_rec();
        too_short();
        return;
    }
    int16_t *pcm = heap_caps_realloc(s_rec, s_rec_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_held[s_held_count++] = (held_note_t){
        .pcm = pcm ? pcm : s_rec,
        .frames = s_rec_n,
        .at_us = esp_timer_get_time(),
        .tries = tried,
    };
    s_rec = NULL;
    if (tried) {
        back_off();
    }
    ESP_LOGI(TAG, "saved a %.1fs note to send later (%d waiting)", (double)s_rec_n / TIMMY_AUDIO_RATE, s_held_count);
    (void)tried;
    go_idle(SAVED_NOTICE);
}

static void drop_oldest(void)
{
    free(s_held[0].pcm);
    s_held_count--;
    memmove(s_held, s_held + 1, s_held_count * sizeof(s_held[0]));
    if (!s_held_count) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
}

typedef enum {
    FED,
    FEED_FAILED,    /* the turn failed, or Hatch stopped taking audio */
    FEED_PRESSED,   /* a press is queued: it goes first */
} feed_t;

/*
 * Gives Hatch a note from *sent on, waiting for room as it goes, then ends
 * the turn. Stops for a press only if `yield`.
 */
static feed_t feed_rest(const int16_t *pcm, size_t frames, size_t *sent, bool yield)
{
    char text[96];
    int64_t moved = esp_timer_get_time();
    for (;;) {
        timmy_hatch_ev_t ev;
        while ((ev = timmy_hatch_turn_event(text, sizeof(text))) != TIMMY_HATCH_EV_NONE) {
            if (ev == TIMMY_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "timmy: %s", text);
                return FEED_FAILED;
            }
        }
        if (*sent == frames) {
            break;
        }
        if (yield && press_waiting()) {
            return FEED_PRESSED;
        }
        size_t n = timmy_hatch_turn_audio_wait(pcm + *sent, frames - *sent, 100);
        *sent += n;
        int64_t now = esp_timer_get_time();
        if (n) {
            moved = now;
        } else if (now - moved > STALL_US) {
            ESP_LOGW(TAG, "Timmy stopped taking the note");
            return FEED_FAILED;
        }
    }
    timmy_hatch_turn_end();
    return FED;
}

/* Asleep: waits for the VM to have the note just sent. */
static bool wait_delivered(void)
{
    char text[96];
    int64_t give_up = esp_timer_get_time() + ACK_WAIT_US;
    while (esp_timer_get_time() < give_up) {
        timmy_hatch_ev_t ev;
        while ((ev = timmy_hatch_turn_event(text, sizeof(text))) != TIMMY_HATCH_EV_NONE) {
            if (ev == TIMMY_HATCH_EV_SENT || ev == TIMMY_HATCH_EV_REPLY || ev == TIMMY_HATCH_EV_DONE) {
                return true;
            }
            if (ev == TIMMY_HATCH_EV_ERROR) {
                ESP_LOGW(TAG, "timmy: %s", text);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "no word that the note arrived");
    return false;
}

/*
 * Sends the oldest saved note. Awake, it's a turn like any other, reply and
 * all; asleep (quiet), it's done once the VM has the note, and the reply
 * waits in the app. A press while it goes up comes first, and the note stays
 * saved. Returns true if a press interrupted the reply.
 */
static bool send_held(bool quiet)
{
    held_note_t *h = &s_held[0];
    /* Timmy has no app to keep an answer in: wake the screen for it. A
     * question older than OLD_ANSWER_US is answered silently, on screen, until
     * a tap, so a late answer doesn't startle anyone. */
    bool old = esp_timer_get_time() - h->at_us > OLD_ANSWER_US;
    if (quiet) {
        timmy_state_set_asleep(false);
        quiet = false;
    }
    s_reply_silent = old;
    ESP_LOGI(TAG, "sending a saved note%s: %.1fs, from %llds ago, try %d", quiet ? " (asleep)" : "",
             (double)h->frames / TIMMY_AUDIO_RATE, (long long)((esp_timer_get_time() - h->at_us) / 1000000),
             h->tries + 1);
    if (!quiet) {
        timmy_state_set_mode(TIMMY_MODE_THINKING);
        status_caption("SENDING SAVED NOTE");
    }
    timmy_hatch_turn_begin();
    size_t sent = 0;
    feed_t fed = feed_rest(h->pcm, h->frames, &sent, true);
    bool delivered = false, interrupted = false;
    if (fed == FED && quiet) {
        delivered = wait_delivered();
    } else if (fed == FED) {
        interrupted = hatch_reply(&delivered, false);
    }
    if (fed != FED || quiet) {
        timmy_hatch_turn_cancel();   /* asleep, the reply is left for the app */
    }
    if (fed == FEED_PRESSED) {
        if (!quiet) {
            go_idle("");
        }
        return false;
    }
    const char *caption;
    bool was_silent = s_reply_silent;
    s_reply_silent = false;
    if (delivered || interrupted) {
        drop_oldest();
        s_next_send_us = 0;   /* the next one right away */
        s_send_backoff_us = RETRY_MIN_US;
        if (!interrupted) {
            if (was_silent) {
                s_notice = true;   /* the answer's caption stays until a tap */
                timmy_state_set_progress(0);
                timmy_state_set_mode(TIMMY_MODE_IDLE);
            } else {
                timmy_state_make_happy();
                go_idle("");
            }
        }
        if (quiet) {
            status_caption("SAVED NOTE SENT");
        }
        return interrupted;
    } else if (++h->tries >= HELD_TRIES) {
        ESP_LOGW(TAG, "giving up on a saved note after %d tries", h->tries);
        drop_oldest();
        caption = "COULDN'T SEND A SAVED NOTE";
    } else {
        back_off();
        caption = STATUS("SAVED NOTE: WILL TRY AGAIN");
    }
    if (quiet) {
        timmy_state_set_caption("%s", caption);
    } else {
        go_idle(caption);
    }
    return false;
}

/*
 * Whether to send a saved note now: Hatch within reach and the backoff over.
 * Coming within reach starts the backoff over. Keeps s_waiting too.
 */
static bool held_due(void)
{
    static bool was_ready = true;
    if (!s_held_count) {
        was_ready = true;
        s_waiting = false;
        return false;
    }
    int64_t now = esp_timer_get_time();
    s_waiting = now - s_held[0].at_us < HELD_KEEP_US;
    bool ready = timmy_hatch_ready();
    if (ready && !was_ready) {
        s_next_send_us = 0;
        s_send_backoff_us = RETRY_MIN_US;
    }
    was_ready = ready;
    return ready && now >= s_next_send_us;
}
#endif

/*
 * After the release: the rest of the note goes to Hatch and the reply plays,
 * or if it can't go now it's saved to send later. Returns true if a press
 * interrupted the reply.
 */
static bool finish_note(void)
{
    bool delivered;
#if HOLD_NOTES
    if (s_rec) {
        bool fed = false, interrupted = false;
        delivered = false;
        if (s_live && s_sent == s_rec_n) {
            timmy_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
            fed = true;
        }
        timmy_audio_chirp(0);
        if (s_live && !fed) {
            /* Hatch is behind (still connecting, say): the rest from the kept note. */
            timmy_state_set_mode(TIMMY_MODE_THINKING);
            status_caption("SENDING VOICE NOTE");
            fed = feed_rest(s_rec, s_rec_n, &s_sent, false) == FED;
            if (!fed) {
                timmy_hatch_turn_cancel();
            }
        }
        if (fed) {
            interrupted = hatch_reply(&delivered, false);
        }
        if (delivered || interrupted) {
            drop_rec();
        } else {
            hold_rec(s_tried);
        }
        return interrupted;
    }
#endif
    timmy_hatch_turn_end();   /* before the chirp, which takes ~90 ms */
    timmy_audio_chirp(0);
    return hatch_reply(&delivered, false);
}

/*
 * Whether a press can record: Hatch within reach, or (HOLD_NOTES) set up
 * with room to save the note. If not, the caption says why.
 */
static bool can_record(void)
{
    if (!timmy_wifi_connected()) {
        timmy_wifi_apply();   /* retry now, not after the backoff */
    }
    bool ready = timmy_hatch_ready();
#if HOLD_NOTES
    timmy_hatch_status_t st;
    timmy_hatch_status(&st);
    if (st.state == TIMMY_HATCH_NOT_SET) {
        go_idle("SET UP " ASSISTANT " FIRST");
        return false;
    }
    if ((!ready || s_held_count) && s_held_count >= HELD_MAX) {
        go_idle(FULL_NOTICE);
        return false;
    }
    if (ready && s_held_count) {
        s_next_send_us = 0;   /* the saved ones go first, right after */
    }
    s_rec = heap_caps_malloc(MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rec) {
        ESP_LOGE(TAG, "no memory to keep a note");
    }
#endif
    if (!ready && !s_rec) {
        go_idle(not_ready_reason());
        return false;
    }
    return true;
}


/* While a toggled note records, the buttons treat talk as held: no menu or sleep. */
static void note_toggled(bool on)
{
#if NOTE_TOGGLE
    timmy_input_note_toggled(on);
#else
    (void)on;
#endif
}

static void voice_task(void *arg)
{
    bool pending_down = false;
    timmy_audio_selftest();
    for (;;) {
        bool wake = false;
        if (!pending_down) {
            timmy_input_event_t ev;
            bool asleep = timmy_state_asleep();
            bool battery = timmy_state_on_battery();
            bool rest = asleep && battery && !s_chirp && !s_mp3test && !s_loopback;
#if HOLD_NOTES
            /* A press goes first: send_held() leaves it queued and returns
             * without backing off, so retrying before it's read would spin. */
            if (held_due() && !press_waiting()) {
                set_resting(false);   /* full power while it goes, even asleep */
                timmy_wifi_power(TIMMY_WIFI_FULL);
                pending_down = send_held(asleep);
                pre_reset();
                if (!pending_down && !asleep && timmy_state_mode(NULL) != TIMMY_MODE_IDLE) {
                    timmy_state_make_happy();
                    go_idle("");
                }
                continue;
            }
#endif
            set_resting(rest);
            /* On battery Wi-Fi rests with the codecs, and between turns dozes
             * between beacons as xiaozhi's idle does; a press wakes it fully. */
            timmy_wifi_power(rest ? TIMMY_WIFI_REST : battery ? TIMMY_WIFI_DOZE : TIMMY_WIFI_FULL);
            if (rest) {
                /* Wait for waking, USB power or a bench request without
                 * polling (the timeout is only a backstop, or a look for
                 * Wi-Fi to send saved notes), so the CPU can stay asleep. */
                timmy_state_wait_awake(s_waiting ? HELD_POLL_MS : REST_BACKSTOP_MS);
                continue;
            }
            if (s_chirp) {
                s_chirp = false;
                timmy_audio_chirp(1);
                pre_reset();
            }
            if (s_mp3test) {
                s_mp3test = false;
                int16_t *pcm = NULL;
                size_t n = timmy_hatch_mp3_selftest(&pcm);
                for (size_t i = 0; i < n; i += TIMMY_AUDIO_CHUNK) {
                    timmy_audio_write(pcm + i, n - i < TIMMY_AUDIO_CHUNK ? n - i : TIMMY_AUDIO_CHUNK);
                }
                free(pcm);
                pre_reset();
            }
            if (s_loopback) {
                s_loopback = false;
                timmy_audio_loopback_test(timmy_settings_volume());
                pre_reset();
            }
            /* A press goes first (a waking one too: it may only wake Timmy). */
            timmy_input_event_t next;
            if (xQueuePeek(s_queue, &next, 0) != pdTRUE && timmy_hatch_incoming()) {
                /* Timmy took long and has the answer now: light the screen, say it. */
                bool delivered;
                timmy_state_set_asleep(false);
                timmy_wifi_power(TIMMY_WIFI_FULL);
                /* Timmy speaking unasked; a late one is shown quietly, kept until a tap. */
                s_reply_silent = timmy_timmy_push_silent();
                pending_down = hatch_reply(&delivered, true);
                bool quiet = s_reply_silent;
                s_reply_silent = false;
                pre_reset();
                if (!pending_down && quiet && delivered) {
                    s_notice = true;
                    timmy_state_set_progress(0);
                    timmy_state_set_mode(TIMMY_MODE_IDLE);
                } else if (!pending_down && timmy_state_mode(NULL) != TIMMY_MODE_IDLE) {
                    go_idle("");
                }
                continue;
            }
            /* The 20 ms read paces this loop. */
            idle_capture();
            if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) {
                continue;
            }
            timmy_state_poke();
            if (ev.type != TIMMY_PTT_DOWN || ev.epoch != timmy_input_call_epoch()) {
                timmy_input_clear_call_pending(ev.mark);
                continue;
            }
            s_press_input_epoch = ev.epoch;
            s_press_mark = ev.mark;
            s_press_us = ev.at_us;
            wake = ev.wake;
        }
        bool ptt = timmy_voice_ptt_enabled();
        if (wake && ptt && !ptt_held_on_waking()) {
            /* A tap: it only woke Timmy. A press posted since keeps its own mark. */
            timmy_input_clear_call_pending(s_press_mark);
            continue;
        }
        if (wake && NOTE_TOGGLE) {
            continue;   /* a waking press only wakes: the next press starts the note */
        }
        note_result_t result = NOTE_CANCELLED;
        if (wake && !ptt) {
            /* A waking press only wakes until it qualifies as a note: no
             * readiness check, call, mode or caption change before that. */
            timmy_input_clear_call_pending(s_press_mark);
            result = note_held(true, false);
            if (result != NOTE_ACCEPTED) {
                pre_reset();
                timmy_state_set_level(0);
                if (result == NOTE_CAPTURE_ERROR && !atomic_load(&s_shutdown)
                    && timmy_state_mode(NULL) != TIMMY_MODE_OFF) {
                    go_idle("MICROPHONE ERROR - TRY AGAIN");
                }
                continue;
            }
        }
        if (atomic_load(&s_shutdown)) {
            pending_down = false;
            timmy_input_clear_call_pending(s_press_mark);
            continue;
        }
        timmy_wifi_power(TIMMY_WIFI_FULL);
        timmy_input_clear_call_pending(s_press_mark);
        if (!can_record()) {
            pending_down = false;
            pre_reset();
            continue;
        }
        note_toggled(true);
        if (!wake) result = note_held(false, pending_down);
        pending_down = false;
        if (result != NOTE_ACCEPTED) {
            note_toggled(false);
            drop_rec();
            pre_reset();
            timmy_state_set_level(0);
            if (atomic_load(&s_shutdown) || timmy_state_mode(NULL) == TIMMY_MODE_OFF) continue;
            if (result == NOTE_CAPTURE_ERROR) go_idle("MICROPHONE ERROR - TRY AGAIN");
            else if (result == NOTE_CANCELLED) go_idle("");
            else too_short();
            continue;
        }
        char why[96];
        bool ok = record(why, sizeof(why));
        note_toggled(false);
        if (!ok) {
            pre_reset();
            go_idle(why);   /* Hatch failed it while recording */
            continue;
        }
        pending_down = finish_note();
        pre_reset();
        if (!pending_down && timmy_state_mode(NULL) != TIMMY_MODE_IDLE) {
            timmy_state_make_happy();
            go_idle("");
        }
    }
}

esp_err_t timmy_voice_start(QueueHandle_t queue)
{
    s_queue = queue;
    s_pre = heap_caps_malloc(PRE_CAP_CHUNKS * sizeof(pre_chunk_t), TIMMY_BIG_CAPS);
    if (!s_pre || timmy_audio_init(timmy_settings_volume(), timmy_settings_mic_gain()) != ESP_OK) {
        timmy_state_set_mode(TIMMY_MODE_ERROR);
        timmy_state_set_caption("AUDIO INIT FAILED");
        return ESP_FAIL;
    }
    /* Stack in PSRAM if there is any (this task never writes flash) to spare internal RAM for Wi-Fi/BLE. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_task, "timmy_voice", 6144, NULL, 6, NULL, TIMMY_AUDIO_CORE,
                                        TIMMY_BIG_CAPS) != pdPASS) {
        /* Without the task nothing would ever rest the codecs, which
         * timmy_audio_init() left open: on the Watcher that's ~33 mA. */
        timmy_audio_power(false);
        timmy_state_set_mode(TIMMY_MODE_ERROR);
        timmy_state_set_caption("AUDIO INIT FAILED");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void timmy_voice_set_monitor(bool on)
{
    s_monitor_db = -100.0f;
    s_monitor = on;
}

float timmy_voice_monitor_db(void)
{
    return s_monitor_db;
}

void timmy_voice_request_chirp(void)
{
    s_chirp = true;
    timmy_state_nudge();
}

void timmy_voice_request_loopback(void)
{
    s_loopback = true;
    timmy_state_nudge();
}

void timmy_voice_request_mp3test(void)
{
    s_mp3test = true;
    timmy_state_nudge();
}

bool timmy_voice_resting(void)
{
    return s_resting;
}

bool timmy_voice_notes_waiting(void)
{
    return s_waiting;
}

int timmy_voice_saved_count(void)
{
#if HOLD_NOTES
    return s_held_count;
#else
    return 0;
#endif
}

bool timmy_voice_notice(void)
{
    return s_notice;
}

void timmy_voice_notice_clear(void)
{
    if (s_notice) {
        s_notice = false;
        timmy_state_set_caption("%s", "");
    }
}
