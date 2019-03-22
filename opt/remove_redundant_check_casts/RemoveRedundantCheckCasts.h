/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "Pass.h"

namespace check_casts {

class RemoveRedundantCheckCastsPass : public Pass {
 public:
  RemoveRedundantCheckCastsPass() : Pass("RemoveRedundantCheckCastsPass") {}

  void configure_pass(const JsonWrapper& jw) override{};
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t remove_redundant_check_casts(DexMethod* method);
};

} // namespace check_casts
