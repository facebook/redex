/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <ostream>

// Streaming support for std::optional, mirroring boost::optional_io behavior.
// Outputs "--" for nullopt, or " " followed by the value.
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& opt) {
  if (opt) {
    os << ' ' << *opt;
  } else {
    os << "--";
  }
  return os;
}
