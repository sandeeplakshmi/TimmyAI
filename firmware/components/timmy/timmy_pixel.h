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
#include <stdint.h>

#include "timmy_state.h"

/*
 * Procedural pixel-art renderer for the Timmy character.
 *
 * Timmy is drawn on a coarse TIMMY_PX_W x TIMMY_PX_H grid with a small palette,
 * ordered dithering and hard outlines, then blown up to the screen with
 * nearest-neighbour blocks so the pixels stay chunky.
 */

#define TIMMY_PX_W 64
#define TIMMY_PX_H 64

typedef struct {
    timmy_mode_t mode;
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in current mode */
    float level;     /* 0..1 live audio level */
    float happy;     /* 0..1 pet reaction */
    float working;   /* 0 or 1 in THINKING: a call's Timmy is busy on work it was asked
                      * for, nothing playing. A renderer that ignores it shows THINKING. */
    /* Extras a renderer may use or ignore (zero when unknown). */
    float touch;     /* 1 while a finger is on the avatar */
    float touch_x, touch_y;   /* where, -1..1 across the avatar (y down) */
    float pet;       /* 0..1 while it's being stroked */
    float dizzy;     /* 0..1 after a flurry of pokes or a shake */
    float tilt_x, tilt_y;     /* -1..1, which way the board leans (gravity on the screen) */
    float idle_s;    /* seconds idle, for drowsiness */
    float offline;   /* 1 while the assistant can't be reached */
    int mood;        /* TIMMY_MOOD_*: the assistant's tone for its reply */
    float mood_t;    /* seconds since mood was set */
    float waiting;   /* 1 while questions asked offline wait to be sent */
    float notice;    /* 1 while an answer waits on screen to be read (a tap clears it) */
    /* A look asked for by the assistant's skills (focus timer, weather, night, nudges). */
    int look;        /* TIMMY_LOOK_*, NONE for the normal face */
    float look_t;    /* seconds it has shown */
    float look_hue;  /* -1, or the colour (0..360) while it shows */
    int particles;   /* TIMMY_PARTICLES_* drifting around while it shows */
    float ring;      /* -1, or the countdown arc left, 1..0 */
} timmy_pose_t;

enum {
    TIMMY_LOOK_NONE, TIMMY_LOOK_HAPPY, TIMMY_LOOK_CHEER, TIMMY_LOOK_SURPRISED, TIMMY_LOOK_SQUINT, TIMMY_LOOK_DROOP,
    TIMMY_LOOK_SHIVER, TIMMY_LOOK_CALM, TIMMY_LOOK_GLOW, TIMMY_LOOK_FOCUS, TIMMY_LOOK_SLEEPY, TIMMY_LOOK_ASLEEP,
    TIMMY_LOOK_POUT, TIMMY_LOOK_COUNT,
};

enum {
    TIMMY_PARTICLES_NONE, TIMMY_PARTICLES_SUN, TIMMY_PARTICLES_RAIN, TIMMY_PARTICLES_SNOW, TIMMY_PARTICLES_HEARTS,
    TIMMY_PARTICLES_STARS, TIMMY_PARTICLES_CONFETTI, TIMMY_PARTICLES_Z, TIMMY_PARTICLES_DROPS, TIMMY_PARTICLES_COUNT,
};

enum {
    TIMMY_MOOD_NONE,
    TIMMY_MOOD_HAPPY,
    TIMMY_MOOD_EXCITED,
    TIMMY_MOOD_CURIOUS,
    TIMMY_MOOD_SAD,
    TIMMY_MOOD_LOVE,
    TIMMY_MOOD_LAUGHING,
    TIMMY_MOOD_SURPRISED,
    TIMMY_MOOD_COUNT,
};

/* Optional: true when the renderer changes smoothly all over each frame, so
 * the UI redraws the whole avatar instead of comparing 64 px cells. */
bool timmy_pixel_full_frame(void);

/* Accent colour of a mode (for the surrounding UI), as 0xRRGGBB. */
uint32_t timmy_pixel_accent(timmy_mode_t mode);

/* Optional: the accent of the working pose (pose->working), as 0xRRGGBB, so
 * the UI around the avatar matches it. 0, or a renderer without it (the UI
 * links a weak default), keeps THINKING's accent. */
uint32_t timmy_pixel_working_accent(void);

/* Render one frame into Timmy's own TIMMY_PX_W x TIMMY_PX_H grid. */
void timmy_pixel_render(const timmy_pose_t *pose);

/* Size (square, in screen pixels) timmy_pixel_scale() blows the grid up to. */
void timmy_pixel_set_size(int px);

/*
 * Write screen pixels [x0, x1] x [y0, y1] of the blown-up frame as RGB565,
 * stride_px apart. Cheap enough to call per display strip, so the full-size
 * image never has to exist in RAM.
 */
void timmy_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
