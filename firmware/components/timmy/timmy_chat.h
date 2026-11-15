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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Talking to Timmy.
 *
 * Timmy holds one HTTP-over-Noise connection to its VM (the transport
 * Home Link uses) and runs each push-to-talk turn over it, entirely on the VM:
 *   speech -> POST /api/voice/dictation (streamed 24 kHz PCM, NDJSON transcripts)
 *   text   -> POST /chat/stream, reply events on POST /chat/subscribe
 *   reply  -> text, shown at reading pace (start_tts in timmy_chat_session.cpp
 *             is where a TTS API of your own would plug in)
 *
 * Credentials: a device token, exchanged through the Timmy API for the VM's own
 * token. If the account API rejects it and a VM ID is set, the token is tried
 * as that VM's token directly.
 *
 * Without CONFIG_TIMMY_HATCH (no PSRAM) the same API is backed by
 * timmy_chat_link.c: the note rides Home Link's session to its paired VM and
 * replies come back as text only (no audio from turn_read). Captions retain
 * up to 1,023 UTF-8 bytes; at most two completed messages await processing.
 * Bursts can omit older messages; subscription lines above 12 KiB are skipped.
 * Use the Timmy app for the full conversation.
 */

typedef enum {
    TIMMY_HATCH_NOT_SET,       /* no token */
    TIMMY_HATCH_OFFLINE,       /* configured, no Wi-Fi */
    TIMMY_HATCH_UNTESTED,
    TIMMY_HATCH_TESTING,
    TIMMY_HATCH_REACHABLE,     /* connected to the VM */
    TIMMY_HATCH_UNREACHABLE,
} timmy_hatch_state_t;

typedef struct {
    timmy_hatch_state_t state;
    char detail[48];
    char vm_id[64];     /* the VM it last connected to; empty before that */
} timmy_hatch_status_t;

/* Starts the connection task; it connects once Wi-Fi is up. */
void timmy_hatch_start(void);

/* The session task connects once Wi-Fi is up, and again whenever a turn needs it. */
void timmy_hatch_status(timmy_hatch_status_t *out);
/* (Re)connects to the VM and reports the result in the status. */
void timmy_hatch_test(void);
/* Call when host/VM/token change: forgets the last result and the connection. */
void timmy_hatch_config_changed(void);
const char *timmy_hatch_state_name(timmy_hatch_state_t state);
/* Screen off, voice idle: check the connection less often. */
void timmy_hatch_set_resting(bool resting);

/* ---- Push-to-talk turns (voice task only) ---- */

/* Token set and Wi-Fi up: a press can start a turn. */
bool timmy_hatch_ready(void);

/* The mood (TIMMY_MOOD_*, timmy_pixel.h) Timmy gave its reply, and how long
 * ago, for the avatar; TIMMY_MOOD_NONE between replies. */
int timmy_timmy_mood(float *age_s);
/* The look Timmy's skills asked for (false: none), and the pout delay; a push to play quietly. */
bool timmy_timmy_look(int *look, float *t, float *hue, int *particles, float *ring, float *pout_after);
bool timmy_timmy_push_silent(void);

/* Press: connects if needed and starts streaming speech to the VM. */
void timmy_hatch_turn_begin(void);
/* 16 kHz mono speech, in order, from press to release. */
void timmy_hatch_turn_audio(const int16_t *pcm, size_t frames);
/* Release: no more audio; the reply follows. */
void timmy_hatch_turn_end(void);
/* Abandons the turn (tap, or barge-in during the reply). */
void timmy_hatch_turn_cancel(void);

typedef enum {
    TIMMY_HATCH_EV_NONE,
    TIMMY_HATCH_EV_HEARD,    /* transcript so far (partial while talking, then final) */
    TIMMY_HATCH_EV_REPLY,    /* reply text so far */
    TIMMY_HATCH_EV_DONE,     /* turn ended; empty text = complete, nonempty text = incomplete reason;
                            * drain remaining audio after either outcome */
    TIMMY_HATCH_EV_ERROR,    /* turn failed; text says why */
    TIMMY_HATCH_EV_SENT,     /* the VM has acknowledged the note */
} timmy_hatch_ev_t;

/* Non-blocking; copies the event's text. Events of cancelled turns are dropped. */
timmy_hatch_ev_t timmy_hatch_turn_event(char *text, size_t cap);

/*
 * For the voice task, while idle. True once a reply has come for a request
 * whose turn ended before it (Timmy said it was still working, and the turn
 * said so and let go); it then starts that reply's turn, to be played like any
 * reply (timmy_hatch_turn_event and the rest), though nobody pressed anything.
 */
bool timmy_hatch_incoming(void);

/*
 * The page of reply text holding what's being said after `played` frames of
 * reply speech (sized by timmy_state_page), or before the speech its opening
 * page. False until there's reply text to page.
 */
bool timmy_hatch_turn_caption(size_t played, char *out, size_t cap);

/* Approximate progress at the same playback cursor as captions, 0..1.
 * message receives the current message index, or -1 before playback. Values
 * may be revised as duration becomes known; the voice task stabilizes them.
 * 1 means this message has drained; the ring caps at 99% until the whole
 * turn succeeds. Voice task only.
 * Link-only builds return 0 and -1 (their paging is unchanged). */
float timmy_turn_progress(size_t played, int *message);

/*
 * For a caption font whose letters differ in width: pages also wrap each line
 * to width_px, measuring it with glyph_px (a glyph's advance in pixels,
 * kerned towards the next one, 0 at the line's end) as the label will, so no
 * line wraps again on screen and a page holds exactly its lines. Every reply
 * is measured: one with CJK or accented letters also keeps to their font's
 * page (timmy_state_page), but its pages without any show in this font. A
 * NULL glyph_px turns it off. Set before any reply is paged; the full UI sets
 * it again when the speaker switches its reply layout, and pages after that
 * use the new width.
 */
void timmy_chat_set_caption_px(int width_px, int (*glyph_px)(uint32_t letter, uint32_t next));

/* Reply speech as 16 kHz mono. Waits up to wait_ms for some; returns frames read. */
size_t timmy_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms);

/* Bench test: decodes a built-in MP3 to 16 kHz; caller frees *pcm. Returns frames. */
size_t timmy_hatch_mp3_selftest(int16_t **pcm);

/*
 * As timmy_hatch_turn_audio, for a note recorded before the turn began: takes
 * what fits of it, waiting up to wait_ms for room if not all of it does, and
 * returns the frames taken (CONFIG_TIMMY_HATCH only).
 */
size_t timmy_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms);

/* ---- Typed turns from the serial console (CONFIG_TIMMY_HATCH only) ---- */

/*
 * Sends typed text to the chat. The reply isn't spoken or captioned: it goes to
 * the console as it streams in, however long it is, one line per piece:
 *   @chat {"seq":N,"type":TYPE,...}
 * TYPE is "sent" (bytes), "busy" (on: the agent is working), "text" (msg, text),
 * "message_done" (msg, bytes), "done" (messages, complete) or "error" (text).
 * tools/timmy/chat.py reads them. Takes `text` (malloc'd) and frees it. A voice
 * press ends a typed turn; a typed turn is refused while a voice turn runs.
 */
void timmy_hatch_text_turn(char *text);
/* Timmy builds (CONFIG_TIMMY_SERVER): serial setup commands after '>' (timmy.url=, timmy.host=,
 * timmy.key=, timmy.device=, timmy). True if the line was one. */
bool timmy_server_console(const char *line);
void timmy_hatch_text_cancel(void);

/*
 * Prints one "@chat" line per call (more if `text` is long): the type, then the
 * printf-style `fields` (JSON members, or NULL), then `text` escaped (or none).
 */
void timmy_hatch_console(const char *type, const char *text, const char *fields, ...)
    __attribute__((format(printf, 3, 4)));
/* Undoes the escapes in a line typed at the console (\n \r \t \\) in place; returns the length. */
size_t timmy_hatch_unescape(char *s);

#ifdef __cplusplus
}
#endif
