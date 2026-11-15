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

#include "timmy_lang.h"

#include <stddef.h>
#include <string.h>

#include "timmy_text.h"
#include "sdkconfig.h"
#include "lvgl.h"

/*
 * A translation is looked up by its English text, so a caption the code
 * changes falls back to English rather than to the wrong words.
 * test_timmy_lang.py checks that each English text is still in the code, that
 * a translation keeps its format conversions and status names their upper
 * case, and that every letter is one its fonts have. A language adds its
 * tables, the Montserrat sizes its letters need and a Kconfig choice. The
 * touch boards' settings pages (timmy_settings_ui.c) aren't translated yet.
 */
typedef struct {
    const char *en, *text;
} timmy_lang_entry_t;

const char *timmy_lang_status(const char *en)
{
    return en;
}

const char *timmy_lang_message(const char *en)
{
    return en;
}

const char *timmy_lang_menu(const char *en)
{
    return en;
}

const lv_font_t *timmy_lang_montserrat(const lv_font_t *font)
{
    return font;
}


const lv_font_t *timmy_lang_label_font(const char *text, const lv_font_t *plain)
{
    (void)text;
    return plain;
}
