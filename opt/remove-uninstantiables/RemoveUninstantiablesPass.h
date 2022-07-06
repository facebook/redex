/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexStore.h"
#include "Pass.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

/// Looks for mentions of classes that have no constructors and use the fact
/// they can't be instantiated to simplify those mentions:
///
///  - If an instance method belongs to an uninstantiable class, its body can be
///    replaced with code that unconditionally throws a NullPointerException.
///  - `instance-of` with an uninstantiable type parameter always returns false.
///  - `invoke-virtual` and `invoke-direct` on methods whose class is
///    uninstantiable can be replaced by code that unconditionally throws a
///    NullPointerException, because they can only be called on a `null`
///    instance.
///  - `check-cast` with an uninstantiable type parameter is equivalent to a
///    a test which throws a `ClassCastException` if the value is not null.
///  - Field accesses on an uninstantiable class can be replaced by code that
///    unconditionally throws a NullPointerException for the same reason as
///    above.
///  - Field accesses returning an uninstantiable class will always return
///    `null`.
class RemoveUninstantiablesPass : public Pass {
 public:
  RemoveUninstantiablesPass() : Pass("RemoveUninstantiablesPass") {}

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
    int check_casts = 0;

    Stats& operator+=(const Stats&);
    Stats operator+(const Stats&) const;

    /// Updates metrics tracked by \p mgr corresponding to these statistics.
    /// Simultaneously prints the statistics via TRACE.
    void report(PassManager& mgr) const;
  };

  static std::unordered_set<DexType*> compute_scoped_uninstantiable_types(
      const Scope& scope,
      std::unordered_map<DexType*, std::unordered_set<DexType*>>*
          instantiable_children = nullptr);

  /// Look for mentions of uninstantiable classes in \p cfg and modify them
  /// in-place.
  static Stats replace_uninstantiable_refs(
      const std::unordered_set<DexType*>& scoped_uninstantiable_types,
      cfg::ControlFlowGraph& cfg);

  /// Replace the instructions in \p cfg with `throw null;`.  Preserves the
  /// initial run of load-param instructions in the ControlFlowGraph.
  ///
  /// \pre Assumes that \p cfg is a non-empty instance method body.
  static Stats replace_all_with_throw(cfg::ControlFlowGraph& cfg);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
