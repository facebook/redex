/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "Pass.h"

class KotlinObjectInliner : public Pass {
  struct Stats {
    size_t kotlin_candidate_companion_objects{0};
    size_t kotlin_untrackable_companion_objects{0};
    size_t kotlin_companion_objects_inlined{0};
    Stats& operator+=(const Stats& that) {
      kotlin_candidate_companion_objects +=
          that.kotlin_candidate_companion_objects;
      kotlin_untrackable_companion_objects +=
          that.kotlin_untrackable_companion_objects;
      kotlin_companion_objects_inlined += that.kotlin_companion_objects_inlined;
      return *this;
    }
    void report(PassManager& mgr) const;
  };

 public:
  void bind_config() override {
    bind("do_not_inline_companion_objects",
         {},
         m_do_not_inline_list,
         "Do not inline these companion objects");
  }
  KotlinObjectInliner() : Pass("KotlinObjectInlinerPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  bool is_editable_cfg_friendly() override { return true; }

 private:
  std::vector<std::string> m_do_not_inline_list;
};
