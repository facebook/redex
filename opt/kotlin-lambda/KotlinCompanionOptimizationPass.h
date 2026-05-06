/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "Pass.h"

class KotlinCompanionOptimizationPass : public Pass {
  struct Stats {
    size_t kotlin_candidate_companion_objects{0};
    size_t kotlin_untrackable_companion_objects{0};
    size_t kotlin_companion_objects_relocated{0};
    size_t kotlin_rejected_not_final{0};
    size_t kotlin_rejected_has_sfields{0};
    size_t kotlin_rejected_has_clinit{0};
    size_t kotlin_rejected_has_interfaces{0};
    size_t kotlin_rejected_has_ifields{0};
    size_t kotlin_rejected_non_object_super{0};
    size_t kotlin_rejected_no_outer_class{0};
    size_t kotlin_rejected_abstract_outer{0};
    size_t kotlin_rejected_invalid_init{0};
    size_t kotlin_rejected_method_uses_this{0};
    Stats& operator+=(const Stats& that) {
      kotlin_candidate_companion_objects +=
          that.kotlin_candidate_companion_objects;
      kotlin_untrackable_companion_objects +=
          that.kotlin_untrackable_companion_objects;
      kotlin_companion_objects_relocated +=
          that.kotlin_companion_objects_relocated;
      kotlin_rejected_not_final += that.kotlin_rejected_not_final;
      kotlin_rejected_has_sfields += that.kotlin_rejected_has_sfields;
      kotlin_rejected_has_clinit += that.kotlin_rejected_has_clinit;
      kotlin_rejected_has_interfaces += that.kotlin_rejected_has_interfaces;
      kotlin_rejected_has_ifields += that.kotlin_rejected_has_ifields;
      kotlin_rejected_non_object_super += that.kotlin_rejected_non_object_super;
      kotlin_rejected_no_outer_class += that.kotlin_rejected_no_outer_class;
      kotlin_rejected_abstract_outer += that.kotlin_rejected_abstract_outer;
      kotlin_rejected_invalid_init += that.kotlin_rejected_invalid_init;
      kotlin_rejected_method_uses_this += that.kotlin_rejected_method_uses_this;
      return *this;
    }
    void report(PassManager& mgr) const;
  };

 public:
  void bind_config() override {
    bind("do_not_relocate_companion_objects",
         {},
         m_do_not_relocate_list,
         "Do not relocate these companion objects");
  }
  KotlinCompanionOptimizationPass() : Pass("KotlinCompanionOptimizationPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {};
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_do_not_relocate_list;
};
