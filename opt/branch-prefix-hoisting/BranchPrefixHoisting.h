/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "ConstantUses.h"
#include "IRList.h"
#include "Lazy.h"
#include "Pass.h"
#include "TypeInference.h"

class IRCode;

namespace cfg {
class Block;
class ControlFlowGraph;
} // namespace cfg

class BranchPrefixHoistingPass : public Pass {
 public:
  BranchPrefixHoistingPass() : Pass("BranchPrefixHoistingPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static size_t process_code(IRCode*, DexMethod*);
  static size_t process_cfg(cfg::ControlFlowGraph&,
                            Lazy<const constant_uses::ConstantUses>&);
};
