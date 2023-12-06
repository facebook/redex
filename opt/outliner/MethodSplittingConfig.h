/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace method_splitting_impl {

struct Config {
  uint64_t split_block_size{100};
  uint64_t min_original_size{5000};
  uint64_t min_original_size_too_large_for_inlining{1500};
  uint64_t min_hot_cold_split_size{300};
  uint64_t min_hot_split_size{400};
  uint64_t min_cold_split_size{500};
  uint64_t huge_threshold{9000};
  float max_overhead_ratio{0.01};
  float max_huge_overhead_ratio{0.02};
  int64_t max_live_in{32};
  uint64_t max_iteration{7};

  // Estimated overhead of having a split method and its metadata.
  size_t cost_split_method{16};
  size_t cost_split_switch{6};
  size_t cost_split_switch_case{4};

  std::vector<std::string> excluded_prefices;
};

} // namespace method_splitting_impl
