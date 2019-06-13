/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace check_casts {

class RemoveRedundantCheckCastsPass : public Pass {
 public:
  RemoveRedundantCheckCastsPass() : Pass("RemoveRedundantCheckCastsPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};

} // namespace check_casts
