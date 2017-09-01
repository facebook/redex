/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstPropV3Config.h"
#include "GlobalConstProp.h"
#include "Pass.h"

class LocalConstantPropagation {
 public:
  using InsnReplaceVector =
      std::vector<std::pair<IRInstruction*, IRInstruction*>>;

  explicit LocalConstantPropagation(const ConstPropV3Config& config)
      : m_branch_propagated{0},
        m_move_to_const{0},
        m_config{config} {}

  void analyze_instruction(IRInstruction* const& insn,
                           ConstPropEnvironment* current_state);
  void simplify_instruction(IRInstruction*& insn,
                            const ConstPropEnvironment& current_state);

  size_t num_branch_propagated() const { return m_branch_propagated; }
  size_t num_move_to_const() const { return m_move_to_const; }

  const InsnReplaceVector& insn_replacements() const {
    return m_insn_replacements;
  }

 private:
  void simplify_branch(IRInstruction*& inst,
                       const ConstPropEnvironment& current_state);
  void analyze_move(IRInstruction* const& inst,
                    ConstPropEnvironment* current_state,
                    bool is_wide);
  void simplify_move(IRInstruction* const& inst,
                    const ConstPropEnvironment& current_state,
                    bool is_wide);
  void propagate(DexMethod* method);

  InsnReplaceVector m_insn_replacements;
  size_t m_branch_propagated;
  size_t m_move_to_const;
  const ConstPropV3Config& m_config;
};

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
