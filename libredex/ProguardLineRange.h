/**
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
};

using ProguardLineRangeVector = std::vector<std::unique_ptr<ProguardLineRange>>;
