/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>

extern bool clean;
extern bool raw;
extern bool escape;

void redump(const char* format, ...);
void redump(uint32_t off, const char* format, ...);
void redump(uint32_t pos, uint32_t off, const char* format, ...);
