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

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoInitClassInstructions, Preserves},
        {NoUnreachableInstructions, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {RenameClass, Preserves},
    };
  }

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static size_t process_code(IRCode*,
                             DexMethod*,
                             bool can_allocate_regs = true);
  static size_t process_cfg(cfg::ControlFlowGraph&,
                            Lazy<const constant_uses::ConstantUses>&,
                            bool can_allocate_regs = true);
};
