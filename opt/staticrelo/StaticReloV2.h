/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace static_relo_v2 {

class StaticReloPassV2 : public Pass {
 public:
  StaticReloPassV2() : Pass("StaticReloPassV2") {}
  static std::unordered_set<DexClass*> gen_candidates(const Scope&);
  static int run_relocation(const Scope&, std::unordered_set<DexClass*>&);
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
} // namespace static_relo_v2
