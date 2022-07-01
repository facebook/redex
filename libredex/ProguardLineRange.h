/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <stdint.h>
#include <string>
#include <vector>
/**
 * ProguardLineRange stores line number values parsed from a proguard mapping.
 */

struct ProguardLineRange final {
  uint32_t start{0};
  uint32_t end{0};
  uint32_t original_start{0};
  uint32_t original_end{0};
  std::string original_name;

  ProguardLineRange() = default;
  ProguardLineRange(
      uint32_t s, uint32_t e, uint32_t os, uint32_t oe, std::string ogn);
  virtual ~ProguardLineRange() = default;

  bool operator==(const ProguardLineRange& other) const;

  // This is an entry of the form "123:321 void foo():523:821 -> a"
  bool remaps_to_range() const;

  // This is an entry of the form "123:321 void foo():5 -> a"
  bool remaps_to_single_line() const;

  bool matches(uint32_t line) const;
};

using ProguardLineRangeVector = std::vector<std::unique_ptr<ProguardLineRange>>;
