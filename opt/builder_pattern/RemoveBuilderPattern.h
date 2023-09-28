/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "Pass.h"

namespace builder_pattern {

class RemoveBuilderPatternPass : public Pass {
 public:
  RemoveBuilderPatternPass() : Pass("RemoveBuilderPatternPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, RequiresAndPreserves},
    };
  }

  explicit RemoveBuilderPatternPass(const std::string& name) : Pass(name) {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  std::unique_ptr<Pass> clone(const std::string& new_name) const override {
    return std::make_unique<RemoveBuilderPatternPass>(new_name);
  }

 private:
  std::vector<DexType*> m_roots;
  std::vector<DexType*> m_blocklist;
  size_t m_max_num_inline_iteration;
};

} // namespace builder_pattern
