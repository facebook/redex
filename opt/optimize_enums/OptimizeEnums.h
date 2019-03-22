/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  void configure_pass(const JsonWrapper& jw) override {}
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};

} // namespace optimize_enums
