/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexStore.h"
#include "Pass.h"

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

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  static std::unordered_set<DexType*> compute_scoped_uninstantiable_types(
      const Scope& scope,
      std::unordered_map<DexType*, std::unordered_set<DexType*>>*
          instantiable_children = nullptr);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
