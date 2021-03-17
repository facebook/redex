/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct ClassSplittingConfig {
  bool enabled{true};
  bool combine_target_classes_by_api_level{false};
  // Relocated methods per target class when combining by API Level.
  unsigned int relocated_methods_per_target_class{64};
  float method_profiles_appear_percent_threshold{0.5f};
  bool relocate_static_methods{true};
  bool relocate_non_static_direct_methods{true};
  bool relocate_non_true_virtual_methods{true};
  bool relocate_true_virtual_methods{true};
  bool run_before_interdex{true};
  bool trampolines{true};
  unsigned int trampoline_size_threshold{100};
  std::vector<std::string> blocklist_types;
};

class ClassSplittingPass : public Pass {
 public:
  ClassSplittingPass() : Pass("ClassSplittingPass") {}

  void bind_config() override {
    bind("enabled", m_config.enabled, m_config.enabled);
    bind("combine_target_classes_by_api_level",
         m_config.combine_target_classes_by_api_level,
         m_config.combine_target_classes_by_api_level);
    bind("relocated_methods_per_target_class",
         m_config.relocated_methods_per_target_class,
         m_config.relocated_methods_per_target_class);
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
    bind("relocate_true_virtual_methods",
         m_config.relocate_true_virtual_methods,
         m_config.relocate_true_virtual_methods);
    bind("run_before_interdex",
         m_config.run_before_interdex,
         m_config.run_before_interdex);
    bind("trampolines", m_config.trampolines, m_config.trampolines);
    bind("trampoline_size_threshold", m_config.trampoline_size_threshold,
         m_config.trampoline_size_threshold);
    bind("blocklist_types",
         {},
         m_config.blocklist_types,
         "List of types for classes to not split.");
    always_assert(!m_config.relocate_true_virtual_methods ||
                  m_config.trampolines);
    always_assert(!m_config.trampolines ||
                  !m_config.combine_target_classes_by_api_level);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  void run_before_interdex(DexStoresVector&, ConfigFiles&, PassManager&);
  ClassSplittingConfig m_config;
};
