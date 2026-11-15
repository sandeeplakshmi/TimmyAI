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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The screen's fonts are LVGL's builds of unscii and Montserrat: ASCII, and
 * Montserrat's symbols. Text from elsewhere (replies, network names) gets
 * ASCII stand-ins for what they lack: curly quotes become straight ones, an em
 * dash "--", accented letters their plain ones. Emoji go; anything else stays,
 * and with CONFIG_TIMMY_CJK_FONT the captions draw CJK from a fallback font.
 * With CONFIG_TIMMY_LATIN_FONT, captions keep their accented letters too.
 */

/* The stand-in for the UTF-8 character at s into out, and its length (0 drops
 * the character); -1 to keep it. *len is the character's length in bytes. */
int timmy_text_ascii(const char *s, size_t *len, char out[4]);

/* CJK runs have no spaces, so a caption line may break between their
 * characters, but not before closing punctuation such as "，" or "。". */
typedef enum {
    TIMMY_TEXT_NOT_CJK,
    TIMMY_TEXT_CJK,          /* a line may break before or after it */
    TIMMY_TEXT_CJK_CLOSE,    /* closing punctuation: never starts a line */
} timmy_text_cjk_t;

/* What the UTF-8 character at s is, for line breaking. */
timmy_text_cjk_t timmy_text_cjk(const char *s);

/* Whether the UTF-8 text s has any CJK in it. */
bool timmy_text_has_cjk(const char *s);

/* Puts the stand-ins into s, which has room for cap bytes. If one doesn't fit,
 * the text ends there. */
void timmy_text_to_ascii(char *s, size_t cap);

/* The caption has a font for accented letters (CONFIG_TIMMY_LATIN_FONT): from
 * now on timmy_text_to_caption() keeps them and timmy_text_has_latin() sees them. */
void timmy_text_keep_latin(void);

/* timmy_text_to_ascii() for a caption, which may keep accented letters. */
void timmy_text_to_caption(char *s, size_t cap);

/* Whether s has accented letters a caption keeps. */
bool timmy_text_has_latin(const char *s);

/* text, or if it needs stand-ins and fits in cap bytes, a copy with them in buf. */
const char *timmy_text_showable(const char *text, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
