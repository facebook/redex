/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <stdint.h>

extern bool clean;
extern bool raw;
extern bool escape;

void redump(const char* format, ...);
void redump(uint32_t off, const char* format, ...);
void redump(uint32_t pos, uint32_t off, const char* format, ...);
