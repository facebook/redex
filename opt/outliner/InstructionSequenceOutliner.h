/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct InstructionSequenceOutlinerConfig {
  size_t min_insns_size{3};
  size_t max_insns_size{77};
  bool use_method_profiles{true};
  float method_profiles_appear_percent{1};
  float method_profiles_hot_call_count{10};
  float method_profiles_warm_call_count{1};
  bool use_perf_sensitive_if_no_method_profiles{true};
  bool reorder_with_method_profiles{true};
  bool reuse_outlined_methods_across_dexes{true};
  size_t max_outlined_methods_per_class{100};
  size_t savings_threshold{10};
};

class InstructionSequenceOutliner : public Pass {
 public:
  InstructionSequenceOutliner() : Pass("InstructionSequenceOutlinerPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector& stores,
                ConfigFiles& cfg,
                PassManager& mgr) override;

 private:
  InstructionSequenceOutlinerConfig m_config;
  size_t m_iteration{0};
};
