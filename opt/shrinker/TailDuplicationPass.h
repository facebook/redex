/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "ShrinkerConfig.h"

namespace tail_duplication_impl {

size_t make_hot_tails_unique(cfg::ControlFlowGraph& cfg,
                             size_t max_block_code_units = 16);

} // namespace tail_duplication_impl

class TailDuplicationPass : public Pass {
 public:
  TailDuplicationPass() : Pass("TailDuplicationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  std::string get_config_doc() override {
    return trim(R"(
If a hot block has multiple predecessor edges, then this pass will "duplicate"
this tail block for each hot predecessor.
This may enable specialization of the tail block for each predecessor, via
const-prop, cse, copy-prop, local-dce, which can improve the efficiency of the
code.
If no specialization happens, then our existing dedup-block functionality will
remove the duplicates again.
Otherwise, the now unique tail blocks will be kept, improving code locality,
increasing the likelihood that the tail block will be selected as the
fallthrough branch, improving processor-level branch prediction.
We use the existing Shrinker to both apply the specialization and to remove the
duplicates.
    )");
  }

  void bind_config() override;
  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  shrinker::ShrinkerConfig m_config;
  size_t m_max_block_code_units;
};
