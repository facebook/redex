/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace optimize_enums {

class OptimizeEnumsPass : public Pass {

 public:
  OptimizeEnumsPass() : Pass("OptimizeEnumsPass") {}
  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  int m_max_enum_size;
  bool m_skip_sanity_check;
  std::vector<DexType*> m_enum_to_integer_allowlist;
};

} // namespace optimize_enums
