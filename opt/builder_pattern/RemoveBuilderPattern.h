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

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<DexType*> m_roots;
  std::vector<DexType*> m_blocklist;
};

} // namespace builder_pattern
