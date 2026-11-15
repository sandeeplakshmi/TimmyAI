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

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Between the status side (timmy_chat.c) and the session task (timmy_chat_session.cpp),
 * plus voice note and caption helpers shared with timmy_chat_link.c. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "timmy_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Token saved (the VM ID is optional) or Link paired to a Hatch account. */
bool timmy_hatch_configured(void);
/* Session task -> status shown in settings and over BLE. */
void timmy_hatch_report(timmy_hatch_state_t state, const char *detail);
/* The VM the session connected to, for the settings page; empty forgets it. */
void timmy_hatch_report_vm(const char *vm_id);

/* Asks the session task to (re)connect now. */
void timmy_hatch_chat_connect(void);
/* Drops the connection and the cached VM credentials (settings changed). */
void timmy_hatch_chat_forget(void);

/* Keep rejected IDs for the turn: later events may omit their parent.
 * On overflow, require a known parent for new messages. */
#define TIMMY_CHAT_REJECTED_MAX 8
typedef struct {
    char ids[TIMMY_CHAT_REJECTED_MAX][80];
    unsigned count;
    bool overflow;
} timmy_chat_rejected_t;

static inline bool timmy_chat_is_rejected(const timmy_chat_rejected_t *rejected, const char *id)
{
    for (unsigned i = 0; i < rejected->count; i++) {
        if (!strcmp(rejected->ids[i], id)) return true;
    }
    return false;
}

static inline void timmy_chat_reject(timmy_chat_rejected_t *rejected, const char *id)
{
    if (!id[0] || timmy_chat_is_rejected(rejected, id)) return;
    size_t len = strlen(id);
    if (rejected->count == TIMMY_CHAT_REJECTED_MAX || len >= sizeof(rejected->ids[0])) {
        rejected->overflow = true;
        return;
    }
    memcpy(rejected->ids[rejected->count++], id, len + 1);
}

/* Identifies spoken turns (voice notes and dictation) as coming from a small
 * microcontroller voice device, so Timmy answers in short plain sentences.
 * Typed turns leave it out. */
#define TIMMY_CHAT_SPOKEN_DEVICE "mcu-wearable"

/* A voice note is a POST /chat/stream body: NOTE_HEAD, a base64 WAV, NOTE_TAIL. */
#define TIMMY_HATCH_NOTE_HEAD \
    "{\"message\":\"\",\"output_modality\":\"text\",\"device\":\"" TIMMY_CHAT_SPOKEN_DEVICE "\"," \
    "\"items\":[{\"type\":\"file\"," \
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\""
#define TIMMY_HATCH_NOTE_TAIL "\"}]}"
#define TIMMY_HATCH_WAV_HEADER 44

/* Voice note helpers (timmy_chat_text.c). The note's length isn't known until
 * the release, so the WAV header gives the streaming "unknown" size. */
void timmy_hatch_wav_header(uint8_t h[TIMMY_HATCH_WAV_HEADER], uint32_t rate);
/* Writes 4 characters per 3 bytes of `in`, padded; returns the length. */
size_t timmy_hatch_base64(const uint8_t *in, size_t n, char *out);

/* Caption text (timmy_chat_text.c): the last line or so of `src`, and the page
 * of wrapped lines holding byte `at` of `text` (false if there's no text). */
void timmy_hatch_tail_words(const char *src, char *out, size_t cap);
bool timmy_hatch_caption_at(const char *text, size_t at, char *out, size_t cap);
/* Manual pages, with the same word/UTF-8 wrapping as automatic captions.
 * Returns the total number of pages; page is clamped to that range. */

#ifdef __cplusplus
}
#endif
