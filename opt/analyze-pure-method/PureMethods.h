/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "InitClassesWithSideEffects.h"
#include "LocalDce.h"
#include "Pass.h"

class AnalyzePureMethodsPass : public Pass {
 public:
  struct Stats {
    size_t number_of_pure_methods_detected{0};
    size_t number_of_pure_methods_invalidated{0};

    Stats& operator+=(const Stats& that) {
      number_of_pure_methods_detected += that.number_of_pure_methods_detected;
      number_of_pure_methods_invalidated +=
          that.number_of_pure_methods_invalidated;
      return *this;
    }

    // Updates metrics tracked by \p mgr corresponding to these statistics.
    void report(PassManager& mgr) const;
  };

  AnalyzePureMethodsPass() : Pass("AnalyzePureMethodsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}}};
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  bool is_editable_cfg_friendly() override { return true; }

  Stats analyze_and_set_pure_methods(Scope& scope);

 private:
  bool analyze_and_check_pure_method_helper(
      const init_classes::InitClassesWithSideEffects&
          init_classes_with_side_effects,
      IRCode* code);
};
