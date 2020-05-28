/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct ClassSplittingConfig {
  unsigned int relocated_methods_per_target_class{64};
  bool use_method_weights{false};
  bool use_method_profiles{true};
  float method_profiles_appear_percent_threshold{1};
};

class ClassSplittingPass : public Pass {
 public:
  ClassSplittingPass() : Pass("ClassSplittingPass") {}

  void bind_config() override {
    bind("relocated_methods_per_target_class",
         m_config.relocated_methods_per_target_class,
         m_config.relocated_methods_per_target_class);
    bind("use_method_weights", m_config.use_method_weights,
         m_config.use_method_weights);
    bind("use_method_profiles", m_config.use_method_profiles,
         m_config.use_method_profiles);
    bind("method_profiles_appear_percent_threshold",
         m_config.method_profiles_appear_percent_threshold,
         m_config.method_profiles_appear_percent_threshold);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ClassSplittingConfig m_config;
};
