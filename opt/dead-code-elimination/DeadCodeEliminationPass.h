/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "SideEffectSummary.h"
#include "UsedVarsAnalysis.h"

class DeadCodeEliminationPass final : public Pass {
 public:
  DeadCodeEliminationPass() : Pass("DeadCodeEliminationPass") {}

  static std::unique_ptr<UsedVarsFixpointIterator> analyze(
      const EffectSummaryMap& effect_summaries,
      const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
      IRCode& code);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
