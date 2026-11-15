/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */

/*
 * Implements timmy_pixel.h by calling the face Settings picked (timmy_avatar.h).
 * Timmy's eyes (avatar/timmy_eyes.c) are built with their functions renamed
 * to timmy_avatar_eyes_* (CMakeLists.txt).
 */

#include "timmy_avatar.h"
#include "timmy_settings.h"

void timmy_avatar_eyes_render(const timmy_pose_t *pose);
void timmy_avatar_eyes_set_size(int px);
void timmy_avatar_eyes_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
uint32_t timmy_avatar_eyes_accent(timmy_mode_t mode);
uint32_t timmy_avatar_eyes_working_accent(void);
bool timmy_avatar_eyes_full_frame(void);

static const timmy_avatar_t EYES = {
    .name = "Timmy",
    .render = timmy_avatar_eyes_render,
    .set_size = timmy_avatar_eyes_set_size,
    .scale = timmy_avatar_eyes_scale,
    .accent = timmy_avatar_eyes_accent,
    .working_accent = timmy_avatar_eyes_working_accent,
    .full_frame = timmy_avatar_eyes_full_frame,
};

__attribute__((weak)) size_t timmy_avatar_extras(const timmy_avatar_t *const **list)
{
    *list = NULL;
    return 0;
}

static const timmy_avatar_t *at(size_t i)
{
    if (i == 0) return &EYES;
    const timmy_avatar_t *const *list;
    size_t n = timmy_avatar_extras(&list);
    return i - 1 < n ? list[i - 1] : &EYES;
}

size_t timmy_avatar_count(void)
{
    const timmy_avatar_t *const *list;
    return 1 + timmy_avatar_extras(&list);
}

const char *timmy_avatar_name(size_t index)
{
    return at(index)->name;
}

static const timmy_avatar_t *current(void)
{
    return at((size_t)timmy_settings_avatar());
}

/* The face the last frame was rendered with, so scale() matches it even if
 * the setting changes between render and scale. */
static const timmy_avatar_t *volatile s_drawn = &EYES;

void timmy_pixel_render(const timmy_pose_t *pose)
{
    const timmy_avatar_t *a = current();
    a->render(pose);
    s_drawn = a;
}

void timmy_pixel_set_size(int px)
{
    for (size_t i = 0; i < timmy_avatar_count(); i++) at(i)->set_size(px);
}

void timmy_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    s_drawn->scale(dst, stride_px, x0, x1, y0, y1);
}

uint32_t timmy_pixel_accent(timmy_mode_t mode)
{
    return current()->accent(mode);
}

uint32_t timmy_pixel_working_accent(void)
{
    const timmy_avatar_t *a = current();
    return a->working_accent ? a->working_accent() : 0;
}

bool timmy_pixel_full_frame(void)
{
    return s_drawn->full_frame ? s_drawn->full_frame() : false;
}
