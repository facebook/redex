/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "DexStore.h"
#include "Pass.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

namespace remove_uninstantiables_impl {

/// Counts of references to uninstantiable classes removed.
struct Stats {
  int instance_ofs = 0;
  int invokes = 0;
  int field_accesses_on_uninstantiable = 0;
  int throw_null_methods = 0;
  int abstracted_classes = 0;
  int abstracted_vmethods = 0;
  int removed_vmethods = 0;
  int get_uninstantiables = 0;
  int invoke_uninstantiables = 0;
  int check_casts = 0;

  int sum() const {
    return instance_ofs + invokes + field_accesses_on_uninstantiable +
           throw_null_methods + abstracted_classes + abstracted_vmethods +
           removed_vmethods + get_uninstantiables + invoke_uninstantiables +
           check_casts;
  }

  Stats& operator+=(const Stats&);
  Stats operator+(const Stats&) const;

  /// Updates metrics tracked by \p mgr corresponding to these statistics.
  /// Simultaneously prints the statistics via TRACE.
  void report(PassManager& mgr) const;
};

/// Look for mentions of uninstantiable classes in \p cfg and modify them
/// in-place.
Stats replace_uninstantiable_refs(
    const std::unordered_set<DexType*>& scoped_uninstantiable_types,
    cfg::ControlFlowGraph& cfg);

/// Replace the instructions in \p cfg with `throw unreachable;`.  Preserves the
/// initial run of load-param instructions in the ControlFlowGraph.
///
/// \pre Assumes that \p cfg is a non-empty instance method body.
Stats replace_all_with_unreachable_throw(cfg::ControlFlowGraph& cfg);

/// Perform structural changes to non-static methods that cannot be called, by
/// either making them abstract, removing their body, or deleting them.
Stats reduce_uncallable_instance_methods(
    const Scope& scope,
    const ConcurrentSet<DexMethod*>& uncallable_instance_methods,
    const std::function<bool(const DexMethod*)>& is_implementation_methods);
} // namespace remove_uninstantiables_impl
