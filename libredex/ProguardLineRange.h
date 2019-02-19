/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <set>
#include <stdint.h>

/**
 * ProguardLineRange stores line number values parsed from a proguard mapping.
 */

struct ProguardLineRange final {
  uint32_t start{0};
  uint32_t end{0};
  uint32_t original_start{0};
  uint32_t original_end{0};

  ProguardLineRange() = default;
  ProguardLineRange(uint32_t s, uint32_t e, uint32_t os, uint32_t oe);
  virtual ~ProguardLineRange() = default;

  bool operator==(const ProguardLineRange& other) const;
};

struct proguardlineranges_comparator {
  bool operator()(std::unique_ptr<ProguardLineRange> const& a,
                  std::unique_ptr<ProguardLineRange> const& b) const {
    return a->start < b->start || a->original_start < b->original_start ||
           a->end < b->end || a->original_end < b->original_end;
  }
};

using ProguardLineRangeSet =
    std::set<std::unique_ptr<ProguardLineRange>, proguardlineranges_comparator>;
