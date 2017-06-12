/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "GlobalConstProp.h"
#include "Pass.h"

class LocalConstantPropagation {
 public:
  LocalConstantPropagation(const Scope& scope,
                           const std::unordered_set<DexType*>& blacklist)
      : m_blacklist(blacklist), m_scope(scope) {}

  void analyze_instruction(IRInstruction* const& insn,
                           ConstPropEnvironment* current_state);
  void simplify_instruction(IRInstruction*& insn,
                            const ConstPropEnvironment& current_state);
  void run();
  size_t num_branch_propagated() const { return m_branch_propagated; }

 private:
  void simplify_branch(IRInstruction*& inst,
                       const ConstPropEnvironment& current_state);
  void analyze_move(IRInstruction* const& inst,
                    ConstPropEnvironment* current_state,
                    bool is_wide);

  const std::unordered_set<DexType*>& m_blacklist;
  const Scope& m_scope;
  std::vector<std::pair<IRInstruction*, IRInstruction*>> m_branch_replacements;
  size_t m_branch_propagated{0};

  void propagate(DexMethod* method);

  // This is temporary and should go away once we combine
  // Intraprocedural constant propagation with this pass.
  void propagate_constants_in_method(DexMethod* method, ControlFlowGraph& cfg);
};

class LocalConstantPropagationPass : public Pass {
 public:
  LocalConstantPropagationPass() : Pass("LocalConstantPropagationPass") {}

  virtual void configure_pass(const PassConfig& pc) override;
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::unordered_set<DexType*> m_blacklist;
};

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
