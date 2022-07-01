/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  Stats analyze_and_set_pure_methods(Scope& scop);

 private:
  bool analyze_and_check_pure_method_helper(IRCode* code);
};
