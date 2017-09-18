/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PrintUtil.h"
#include <stdio.h>
#include <cstdarg>

bool clean = false;
bool raw = false;
bool escape = false;

void redump(const char* format, ...) {
  va_list va;
  va_start(va, format);
  vprintf(format, va);
  va_end(va);
}

void redump(uint32_t off, const char* format, ...) {
  va_list va;
  va_start(va, format);
  if (!clean) printf("[0x%x] ", off);
  vprintf(format, va);
  va_end(va);
}

void redump(uint32_t pos, uint32_t off, const char* format, ...) {
  va_list va;
  va_start(va, format);
  if (!clean) printf("(0x%x) [0x%x] ", pos, off);
  vprintf(format, va);
  va_end(va);
}
