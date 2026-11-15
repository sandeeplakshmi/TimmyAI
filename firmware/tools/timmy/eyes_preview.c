/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */

/*
 * Host preview of Timmy's eyes (avatar/timmy_eyes.c): renders a scene's frames
 * as raw 24-bit RGB on stdout, for tools/timmy/make_eye_gifs.py. No board needed.
 *
 *   cc -O2 -I components/timmy tools/timmy/eyes_preview.c avatar/timmy_eyes.c -lm -o /tmp/eyes
 *   /tmp/eyes SCENE SIZE FRAMES FPS > frames.rgb
 * Scenes: idle listening thinking speaking happy sleepy ring hero
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timmy_pixel.h"

static timmy_pose_t base(float t)
{
    timmy_pose_t p;
    memset(&p, 0, sizeof(p));
    p.mode = TIMMY_MODE_IDLE;
    p.t = t;
    p.mode_t = t;
    p.idle_s = 5;
    p.look_hue = -1;
    p.ring = -1;
    return p;
}

static float speech(float t)   /* a voice-like level: syllables in phrases */
{
    float syl = 0.5f + 0.5f * sinf(t * 2 * 3.14159f * 4.3f);
    float phrase = sinf(t * 2 * 3.14159f * 0.45f) > -0.6f ? 1.0f : 0.15f;
    return fminf(1, syl * phrase * (0.6f + 0.4f * sinf(t * 7.1f) * sinf(t * 2.3f)));
}

static timmy_pose_t scene(const char *name, float t, float dur)
{
    timmy_pose_t p = base(t + 10);
    if (!strcmp(name, "idle")) {
        /* breathing, blinks, glances */
    } else if (!strcmp(name, "listening")) {
        p.mode = TIMMY_MODE_LISTENING; p.mode_t = t + 0.5f; p.level = speech(t) * 0.9f;
    } else if (!strcmp(name, "thinking")) {
        p.mode = TIMMY_MODE_THINKING; p.mode_t = t + 0.5f;
    } else if (!strcmp(name, "speaking")) {
        p.mode = TIMMY_MODE_SPEAKING; p.mode_t = t + 0.5f; p.level = speech(t);
        p.mood = TIMMY_MOOD_HAPPY; p.mood_t = t;
    } else if (!strcmp(name, "happy")) {
        p.happy = t < dur - 0.5f ? 1 : 0; p.pet = (t > 1 && t < dur - 1) ? 1 : 0;
        p.mood = TIMMY_MOOD_LOVE; p.mood_t = t;
    } else if (!strcmp(name, "sleepy")) {
        p.idle_s = 900 + t; p.look = TIMMY_LOOK_SLEEPY; p.look_t = t + 1; p.particles = TIMMY_PARTICLES_Z;
    } else if (!strcmp(name, "ring")) {
        p.look = TIMMY_LOOK_FOCUS; p.look_t = t + 2; p.ring = 0.62f;
    } else if (!strcmp(name, "hero")) {
        /* cycle: idle, listening, thinking, speaking, happy, excited, curious */
        static const int moods[] = { TIMMY_MOOD_NONE, TIMMY_MOOD_NONE, TIMMY_MOOD_NONE, TIMMY_MOOD_HAPPY,
                                     TIMMY_MOOD_LOVE, TIMMY_MOOD_EXCITED, TIMMY_MOOD_CURIOUS };
        float seg = dur / 7;
        int i = (int)(t / seg);
        if (i > 6) i = 6;
        float st = t - i * seg;
        p.mode = i == 1 ? TIMMY_MODE_LISTENING : i == 2 ? TIMMY_MODE_THINKING : i == 3 ? TIMMY_MODE_SPEAKING : TIMMY_MODE_IDLE;
        p.mode_t = st + 0.3f;
        p.level = i == 1 ? speech(t) * 0.9f : i == 3 ? speech(t) : 0;
        p.mood = moods[i]; p.mood_t = st;
        p.happy = i == 4 ? 1 : 0;
    } else {
        fprintf(stderr, "unknown scene %s\n", name);
        exit(2);
    }
    return p;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s SCENE SIZE FRAMES FPS > frames.rgb\n", argv[0]);
        return 2;
    }
    const char *name = argv[1];
    int size = atoi(argv[2]), frames = atoi(argv[3]);
    float fps = (float)atof(argv[4]), dur = frames / fps;
    srand(7);
    uint16_t *buf = malloc((size_t)size * size * 2);
    unsigned char *rgb = malloc((size_t)size * size * 3);
    timmy_pixel_set_size(size);
    for (float t = -1.0f; t < 0; t += 1 / fps) timmy_pixel_render(&(timmy_pose_t){0});   /* settle */
    for (int f = 0; f < frames; f++) {
        timmy_pose_t p = scene(name, f / fps, dur);
        timmy_pixel_render(&p);
        timmy_pixel_scale(buf, size, 0, size - 1, 0, size - 1);
        for (int i = 0; i < size * size; i++) {
            uint16_t c = buf[i];
            rgb[3 * i] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
            rgb[3 * i + 1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
            rgb[3 * i + 2] = (unsigned char)((c & 31) * 255 / 31);
        }
        fwrite(rgb, 1, (size_t)size * size * 3, stdout);
    }
    return 0;
}
