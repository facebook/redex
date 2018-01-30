/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/optional.hpp>

#include "ConstPropConfig.h"
#include "ConstantEnvironment.h"
#include "Pass.h"

class LocalConstantPropagation {
 public:
  using InsnReplaceVector =
      std::vector<std::pair<IRInstruction*, IRInstruction*>>;

  explicit LocalConstantPropagation(
      const ConstPropConfig& config,
      ConstantStaticFieldEnvironment field_env)
      : m_branch_propagated{0},
        m_materialized_consts{0},
        m_config{config},
        m_field_env(field_env) {}

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state);
  void simplify_instruction(IRInstruction*& insn,
                            const ConstantEnvironment& current_state);

  size_t num_branch_propagated() const { return m_branch_propagated; }
  size_t num_materialized_consts() const { return m_materialized_consts; }

  const InsnReplaceVector& insn_replacements() const {
    return m_insn_replacements;
  }

 private:
  void simplify_branch(IRInstruction*& inst,
                       const ConstantEnvironment& current_state);

  using value_transform_t = std::function<boost::optional<int64_t>(int64_t)>;

  static boost::optional<int64_t> identity(int64_t v) { return v; }

  void analyze_non_branch(const IRInstruction* inst,
                          ConstantEnvironment* current_state,
                          value_transform_t value_transform = identity);

  void simplify_non_branch(IRInstruction* inst,
                           const ConstantEnvironment& current_state,
                           bool is_wide);

  void propagate(DexMethod* method);

  InsnReplaceVector m_insn_replacements;
  size_t m_branch_propagated;
  size_t m_materialized_consts;
  const ConstPropConfig& m_config;
  ConstantStaticFieldEnvironment m_field_env;
};

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
