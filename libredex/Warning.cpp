/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Warning.h"

#include <cstdarg>
#include <cstdio>

OptWarningLevel g_warning_level = NO_WARN;

const char* s_warning_text[] = {
#define OPT_WARN(warn, str) str,
    OPT_WARNINGS
#undef OPT_WARN
};

size_t s_warning_counts[] = {
#define OPT_WARN(...) 0,
    OPT_WARNINGS
#undef OPT_WARN
};

constexpr size_t kNumWarnings =
    sizeof(s_warning_counts) / sizeof(s_warning_counts[0]);

void opt_warn(OptWarning warn, const char* fmt, ...) {
  ++s_warning_counts[warn];
  if (g_warning_level == WARN_FULL) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", s_warning_text[warn]);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
}

void print_warning_summary() {
  if (g_warning_level != WARN_COUNT) return;
  for (size_t i = 0; i < kNumWarnings; i++) {
    size_t count = s_warning_counts[i];
    if (count > 0) {
      fprintf(stderr,
              "Optimization warning: %s: %zu occurrences\n",
              s_warning_text[i],
              count);
    }
  }
}
