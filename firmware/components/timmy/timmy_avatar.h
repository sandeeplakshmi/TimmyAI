/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */

/*
 * Avatar registry: the faces the home screen can show. Timmy's eyes are
 * built in. An add-on component (one found through EXTRA_COMPONENT_DIRS, see
 * docs/CUSTOMIZING.md) can add more by defining timmy_avatar_extras(); with
 * more than one face, Settings > Avatar lists them.
 *
 * The selected face implements timmy_pixel.h for the UI (timmy_avatar.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "timmy_pixel.h"

typedef struct {
    const char *name;                                   /* shown in Settings > Avatar */
    void (*render)(const timmy_pose_t *pose);
    void (*set_size)(int px);
    void (*scale)(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
    uint32_t (*accent)(timmy_mode_t mode);
    uint32_t (*working_accent)(void);                   /* optional (NULL) */
    bool (*full_frame)(void);                           /* optional (NULL) */
} timmy_avatar_t;

/* Defined by an add-on to add faces: points *list at an array of entries and
 * returns how many. The default (weak) adds none. The add-on's component must
 * be linked whole (WHOLE_ARCHIVE) so its definition wins. */
size_t timmy_avatar_extras(const timmy_avatar_t *const **list);

/* All faces: index 0 is Timmy's eyes. */
size_t timmy_avatar_count(void);
const char *timmy_avatar_name(size_t index);
