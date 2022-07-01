/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagation.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Pass.h"

class ConstantPropagationPass : public Pass {
 public:
  ConstantPropagationPass() : Pass("ConstantPropagationPass") {}

  void bind_config() override {
    bind("replace_moves_with_consts",
         true,
         m_config.transform.replace_moves_with_consts);
    bind("remove_dead_switch", true, m_config.transform.remove_dead_switch);
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  constant_propagation::Config m_config;
};
