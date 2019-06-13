/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Pass.h"

class ConstantPropagationPass : public Pass {
 public:
  struct Config {
    constant_propagation::Transform::Config transform;
  };

  ConstantPropagationPass() : Pass("ConstantPropagationPass") {}

  void bind_config() override {
    bind("replace_moves_with_consts",
         true,
         m_config.transform.replace_moves_with_consts);
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  Config m_config;
};
