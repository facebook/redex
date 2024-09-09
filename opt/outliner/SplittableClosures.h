/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "MethodClosures.h"

namespace method_splitting_impl {

// Represents a single argument to a method closure. It must either have a type,
// to pass the register value through a parameter, or a simple definition, which
// must be const opcode.
struct ClosureArgument {
  reg_t reg;
  const DexType* type;
  IRInstruction* def;
};

// This data structure either represents the code following a single block, or a
// set of cases of a switch. It is "splittable" as it has been ensured that this
// part of the given method can in fact be split into a separate method.
struct SplittableClosure {
  std::shared_ptr<MethodClosures> method_closures;
  cfg::Block* switch_block;
  std::vector<const Closure*> closures;
  std::vector<ClosureArgument> args;
  double rank;
  size_t added_code_size;
  HotSplitKind hot_split_kind;
  bool is_large_packed_switch{false};
  bool creates_large_sparse_switch{false};
  bool destroys_large_packed_switch{false};

  int is_switch() const { return switch_block ? 1 : 0; }

  // id is unique among all splittable closures where is_switch() is the same.
  size_t id() const {
    if (switch_block) {
      return switch_block->id();
    }
    always_assert(closures.size() == 1);
    return closures.front()->target->id();
  }

  std::vector<DexType*> get_arg_types() const;
};

// Selects splittable closures for a given set of methods based of configured
// costs.
ConcurrentMap<DexType*, std::vector<SplittableClosure>>
select_splittable_closures_based_on_costs(
    const ConcurrentSet<DexMethod*>& methods,
    const Config& config,
    InsertOnlyConcurrentSet<const DexMethod*>* concurrent_hot_methods,
    InsertOnlyConcurrentMap<DexMethod*, size_t>*
        concurrent_splittable_no_optimizations_methods);

// Selects splittable closures for a given set of methods from all contained
// top-level switch cases.
InsertOnlyConcurrentMap<DexMethod*, std::vector<SplittableClosure>>
select_splittable_closures_from_top_level_switch_cases(
    const std::vector<DexMethod*>& methods, size_t max_live_in);

} // namespace method_splitting_impl
