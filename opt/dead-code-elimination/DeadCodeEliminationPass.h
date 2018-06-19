/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/optional.hpp>

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

  virtual void configure_pass(const PassConfig& pc) override {
    std::string external_summaries_file;
    pc.get("external_summaries", "", external_summaries_file);
    if (external_summaries_file != "") {
      m_external_summaries_file = external_summaries_file;
    }
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  boost::optional<std::string> m_external_summaries_file;
};
