/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ProcessUsesNamesAnnoPass : public Pass {
 public:
  struct Metrics {
    int32_t used_classes{0};
    int32_t used_classes_by_subclass{0};
    int32_t used_classes_by_field{0};
    int32_t used_fields{0};
    int32_t used_methods{0};
    int32_t uses_names_anno{0};
    int32_t uses_names_trans_anno{0};
  };

  ProcessUsesNamesAnnoPass() : Pass("ProcessUsesNamesAnnoPass") {}

  void bind_config() override {
    bind("uses_names_annotation",
         DexType::get_type("Lcom/facebook/redex/annotations/UsesNames;"),
         m_uses_names_annotation);
    bind("uses_names_trans_annotation",
         DexType::get_type(
             "Lcom/facebook/redex/annotations/UsesNamesTransitive;"),
         m_uses_names_trans_annotation);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  DexType* m_uses_names_annotation;
  DexType* m_uses_names_trans_annotation;
};
