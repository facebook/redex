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
  using InsnReplaceVector =
      std::vector<std::pair<IRInstruction*, IRInstruction*>>;

  LocalConstantPropagation() : m_branch_propagated(0) {}

  void analyze_instruction(IRInstruction* const& insn,
                           ConstPropEnvironment* current_state);
  void simplify_instruction(IRInstruction*& insn,
                            const ConstPropEnvironment& current_state);
  size_t num_branch_propagated() const { return m_branch_propagated; }
  const InsnReplaceVector& branch_replacements() const {
    return m_branch_replacements;
  }

 private:
  void simplify_branch(IRInstruction*& inst,
                       const ConstPropEnvironment& current_state);
  void analyze_move(IRInstruction* const& inst,
                    ConstPropEnvironment* current_state,
                    bool is_wide);
  void propagate(DexMethod* method);

  InsnReplaceVector m_branch_replacements;
  size_t m_branch_propagated;
};

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
