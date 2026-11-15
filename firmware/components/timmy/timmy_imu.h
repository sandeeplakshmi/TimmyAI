/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */
#pragma once

#include <stdbool.h>

/* Acceleration in g along the board's x, y, z (z out of the screen), or false
 * when there's no accelerometer. Probes on the first call; call from one task. */
bool timmy_imu_accel(float g[3]);
