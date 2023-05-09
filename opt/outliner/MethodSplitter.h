/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "MethodSplittingConfig.h"

class DexStore;
using DexStoresVector = std::vector<DexStore>;

namespace method_splitting_impl {

struct Stats {
  std::atomic<size_t> split_count_simple{0};
  std::atomic<size_t> split_count_switches{0};
  std::atomic<size_t> split_count_switch_cases{0};
  std::atomic<size_t> hot_split_count{0};
  std::atomic<size_t> hot_cold_split_count{0};
  std::atomic<size_t> cold_split_count{0};
  std::atomic<size_t> dex_limits_hit{0};
  std::atomic<size_t> added_code_size{0};
  std::unordered_set<DexMethod*> added_methods;
};

void split_methods_in_stores(DexStoresVector& stores,
                             int32_t min_sdk,
                             const Config& config,
                             bool create_init_class_insns,
                             size_t reserved_mrefs,
                             size_t reserved_trefs,
                             Stats* stats,
                             const std::string& name_infix = "");

} // namespace method_splitting_impl
