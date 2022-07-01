/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintUtil.h"
#include <cstdarg>
#include <stdio.h>

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
