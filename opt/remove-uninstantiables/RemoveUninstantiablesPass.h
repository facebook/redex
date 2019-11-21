/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
///  - `instance-of` with an uninstantiable type parameter always returns false.
///  - `invoke-virtual` and `invoke-direct` on methods whose class is
///    uninstantiable can be replaced by a `throw null;`, because they can only
///    be called on a `null` instance.
///  - Field accesses on an uninstantiable class can be replaced by a `throw
///    null;` for the same reason as above.
///  - Field accesses returning an uninstantiable class will always return
///    `null`.
class RemoveUninstantiablesPass : public Pass {
 public:
  RemoveUninstantiablesPass() : Pass("RemoveUninstantiablesPass") {}

  /// Look for mentions of uninstantiable classes in \p cfg and modify them
  /// in-place.
  static void remove_from_cfg(cfg::ControlFlowGraph& cfg);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
