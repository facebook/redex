/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Debug.h"

// Expands `$NAME`-style placeholders in an IR template.
//
// A test that needs a symbol interpolated into assembled IR should substitute
// into a raw string literal rather than build the text with `+`. An
// s-expression is only readable when its instructions line up one per line,
// and a concatenation splits the text into operands that clang-format is free
// to reflow -- which leaves a ladder of quoted fragments in place of the code
// being assembled.
//
// Placeholders are matched left to right and a replacement is never rescanned,
// so a value containing a `$` (a Kotlin-style nested name, say) cannot be
// corrupted by a later substitution. No placeholder may be a prefix of another.
//
// A placeholder must be non-empty and start with `$`. Both are asserted rather
// than left to the reader: an empty key matches at every position and advances
// the scan by zero, which hangs instead of failing, and a key without the `$`
// can never match, so the template comes back with the placeholder still in it
// and the test asserts against text nobody intended.
inline std::string ir(
    std::string_view tmpl,
    const std::vector<std::pair<std::string_view, std::string>>& subs) {
  for (const auto& sub : subs) {
    always_assert_log(!sub.first.empty() && sub.first.front() == '$',
                      "ir(): placeholder must be non-empty and start with '$'");
  }
  std::string out;
  out.reserve(tmpl.size());
  for (size_t i = 0; i < tmpl.size();) {
    bool matched = false;
    if (tmpl[i] == '$') {
      for (const auto& [key, value] : subs) {
        if (tmpl.compare(i, key.size(), key) == 0) {
          out += value;
          i += key.size();
          matched = true;
          break;
        }
      }
    }
    if (!matched) {
      out += tmpl[i++];
    }
  }
  return out;
}
