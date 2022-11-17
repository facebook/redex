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

std::unordered_map<std::string_view, DexType*> make_deobfuscated_map(const std::unordered_set<DexType*>& obfuscated_types);

class UsageHandler {
 public:
  std::string cls_name;
  std::string type_name;
  DexType* cls_type = nullptr;
  DexType* usage_type = nullptr;
  int count = 0;
  bool has_ctor = false;

  void reset() {
    cls_name = "";
    type_name = "";
    cls_type = nullptr;
    usage_type = nullptr;
    count = 0;
    has_ctor = false;
  };

  UsageHandler() {};

  void handle_usage_line(
    const std::string& line,
    const std::unordered_map<std::string_view, DexType*>& deobfuscated_uninstantiable_type,
    std::unordered_set<DexType*>& uninstantiable_types);
};

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
  UsageHandler uh;
  // Testing injector to mimic m_proguard_usage_name
  static std::istream* test_only_usage_file_input;

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

  static void readUsage(std::istream& usage_file, std::unordered_set<DexType*>& uninstantiable_types);

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

  void bind_config() override {
    // The Proguard usage.txt file name. Need to be added by gradle plugin.
    bind("proguard_usage_name", "", m_proguard_usage_name);
  }

 protected:
  static std::string m_proguard_usage_name;
};
