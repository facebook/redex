/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardLineRange.h"

#include <utility>

ProguardLineRange::ProguardLineRange(
    uint32_t s, uint32_t e, uint32_t os, uint32_t oe, std::string ogn)
    : start(s),
      end(e),
      original_start(os),
      original_end(oe),
      original_name(std::move(ogn)) {}

bool ProguardLineRange::operator==(const ProguardLineRange& other) const {
  return this->start == other.start && this->end == other.end &&
         this->original_start == other.original_start &&
         this->original_end == other.original_end &&
         this->original_name == other.original_name;
}

bool ProguardLineRange::remaps_to_range() const {
  return original_start != 0 && original_end != 0;
}

bool ProguardLineRange::remaps_to_single_line() const {
  return original_start != 0 && original_end == 0;
}

bool ProguardLineRange::matches(uint32_t line) const {
  return start <= line && end >= line;
}
