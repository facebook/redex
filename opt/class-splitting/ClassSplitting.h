/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ClassSplittingPass : public Pass {
 public:
  ClassSplittingPass() : Pass("ClassSplittingPass") {}

  void configure_pass(const JsonWrapper&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_relocated_methods_per_target_class;
};
