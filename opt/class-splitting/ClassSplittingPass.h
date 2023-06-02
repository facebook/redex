/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ClassSplitting.h"
#include "Pass.h"

namespace class_splitting {

class ClassSplittingPass : public Pass {
 public:
  ClassSplittingPass() : Pass("ClassSplittingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, RequiresAndEstablishes},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

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
    bind("trampolines", m_config.trampolines, m_config.trampolines);
    bind("trampoline_size_threshold", m_config.trampoline_size_threshold,
         m_config.trampoline_size_threshold);
    bind("blocklist_types",
         {},
         m_config.blocklist_types,
         "List of types for classes to not split.");
    bind("profile_only", m_config.profile_only, m_config.profile_only);
    always_assert(!m_config.relocate_true_virtual_methods ||
                  m_config.trampolines);
    always_assert(!m_config.trampolines ||
                  !m_config.combine_target_classes_by_api_level);
  }

  bool is_cfg_legacy() override { return true; }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ClassSplittingConfig m_config;
};
} // namespace class_splitting
