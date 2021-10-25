/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Standard copy propagation will not remove `move-object` instructions
// flowing into `monitor` instructions, in an effort to not disturb hopefully
// "good" code that passes Android's lock verification (which has only very
// simplistic alias tracking for performance reasons).
//
// At the same time, leaving in additional `move-object` instructions that
// create aliases can again trigger unexpected verification behavior. This
// case can happen when inlining Java-synchronized callees into synchronized
// callers.
//
// CanonicalizeLocks attempts to detect this case via a simplistic reaching-
// definitions analysis (ignoring cases where the definitions are not
// singletons). If a group of `monitor` instructions does not access a reference
// through the same (intermediate) instruction, a new temporary register is
// introduced to hold the reference for its complete lifetime, and the `monitor`
// instructions are rewritten.
//
// The rewrite introduces a `move-object` immediately after the "source" of a
// group. This will ensure correct lifetime, as well as allow standard copy
// propagation to remove the other `move-object` instructions that created the
// aliases. The expected net benefit is smaller code (as at least one
// `move-object` must exist, and is likely not used otherwise). Judging
// increased register pressure is non-trivial.
//
// WARNING: The `run` method must not be called after register allocation!

#pragma once

#include <cstddef>
#include <unordered_set>

class DexMethod;

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace copy_propagation_impl {
namespace locks {

struct Result {
  bool has_locks{false};
  bool non_singleton_rdefs{false};
  size_t fixups{0};
};

Result run(cfg::ControlFlowGraph& cfg);

} // namespace locks
} // namespace copy_propagation_impl
