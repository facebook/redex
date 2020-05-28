/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct ClassSplittingConfig {
  bool combine_target_classes_by_api_level{false};
  unsigned int relocated_methods_per_target_class{64};
  bool use_method_weights{false};
  bool use_method_profiles{true};
  float method_profiles_appear_percent_threshold{1};
  bool relocate_static_methods{true};
  bool relocate_non_static_direct_methods{true};
  bool relocate_non_true_virtual_methods{true};
  bool run_before_interdex{true};
};

class ClassSplittingPass : public Pass {
 public:
  ClassSplittingPass() : Pass("ClassSplittingPass") {}

  void bind_config() override {
    bind("combine_target_classes_by_api_level",
         m_config.combine_target_classes_by_api_level,
         m_config.combine_target_classes_by_api_level);
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
    bind("relocate_static_methods", m_config.relocate_static_methods,
         m_config.relocate_static_methods);
    bind("relocate_non_static_direct_methods",
         m_config.relocate_non_static_direct_methods,
         m_config.relocate_non_static_direct_methods);
    bind("relocate_non_true_virtual_methods",
         m_config.relocate_non_true_virtual_methods,
         m_config.relocate_non_true_virtual_methods);
    bind("run_before_interdex",
         m_config.run_before_interdex,
         m_config.run_before_interdex);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  void run_before_interdex(DexStoresVector&, ConfigFiles&, PassManager&);
  ClassSplittingConfig m_config;
};
