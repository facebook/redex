/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

bool is_outlined_class(DexClass* cls);

struct InstructionSequenceOutlinerConfig {
  size_t min_insns_size;
  size_t max_insns_size;
  bool use_method_to_weight;
  bool reuse_outlined_methods_across_dexes;
  size_t max_outlined_methods_per_class;
  size_t threshold;
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
};
