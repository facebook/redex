/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <utility>

#include "BaseIRAnalyzer.h"
#include "DexUtil.h"
#include "InitClassDomain.h"
#include "InitClassesWithSideEffects.h"
#include "MonotonicFixpointIterator.h"

namespace init_classes {

class InitClassForwardFixpointIterator final
    : public ir_analyzer::BaseIRAnalyzer<InitClassDomain> {
 private:
  const InitClassesWithSideEffects& m_init_classes_with_side_effects;

 public:
  explicit InitClassForwardFixpointIterator(
      const InitClassesWithSideEffects& init_classes_with_side_effects,
      const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseIRAnalyzer<InitClassDomain>(cfg),
        m_init_classes_with_side_effects(init_classes_with_side_effects) {}

  InitClassDomain initial_env(const DexType* declaring_type) {
    InitClassDomain env;
    env.insert(m_init_classes_with_side_effects, declaring_type);
    return env;
  }

  void analyze_instruction(const IRInstruction* insn,
                           InitClassDomain* current_state) const override {
    auto init_class = ::get_init_class_type_demand(insn);
    current_state->insert(m_init_classes_with_side_effects, init_class);
  }
};

} // namespace init_classes
