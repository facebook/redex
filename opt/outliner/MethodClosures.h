/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>

#include "DeterministicContainers.h"
#include "MethodSplittingConfig.h"
#include "ReducedControlFlow.h"

namespace method_splitting_impl {

// A "closure" here represents the transitive closure of all blocks reachable
// from a given reduced block.
struct Closure {
  const ReducedBlock* reduced_block;
  UnorderedSet<const ReducedBlock*> reduced_components;
  UnorderedSet<cfg::Block*> srcs;
  cfg::Block* target;
};

// A set of closures associated with a particular method.
struct MethodClosures {
  DexMethod* method;
  size_t original_size;
  std::shared_ptr<const ReducedControlFlowGraph> rcfg;
  std::vector<Closure> closures;
};

std::shared_ptr<const ReducedControlFlowGraph> reduce_cfg(
    DexMethod* method, std::optional<uint64_t> split_block_size = std::nullopt);

// Find potentially relevant closures for a method.
std::shared_ptr<MethodClosures> discover_closures(
    DexMethod* method, std::shared_ptr<const ReducedControlFlowGraph> rcfg);

} // namespace method_splitting_impl
