/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
  size_t index;
  std::vector<ClosureArgument> args;
  double rank;
  size_t added_code_size;
  HotSplitKind hot_split_kind;
};

// Selects splittable closures for a given set of methods.
std::unordered_map<DexType*, std::vector<SplittableClosure>>
select_splittable_closures(const std::unordered_set<DexMethod*>& methods,
                           const Config& config);

} // namespace method_splitting_impl
