// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <unordered_set>

#include "Pass.h"

namespace builder_pattern {

class RemoveBuilderPatternPass : public Pass {
 public:
  RemoveBuilderPatternPass() : Pass("RemoveBuilderPatternPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<DexType*> m_roots;
  std::vector<DexType*> m_blocklist;
  bool m_propagate_escape_results;
};

} // namespace builder_pattern
