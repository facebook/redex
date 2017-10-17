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
#include "GlobalConstProp.h"
#include "Pass.h"

class LocalConstantPropagation {
 public:
  using InsnReplaceVector =
      std::vector<std::pair<IRInstruction*, IRInstruction*>>;

  explicit LocalConstantPropagation(const ConstPropConfig& config)
      : m_branch_propagated{0},
        m_materialized_consts{0},
        m_config{config} {}

  void analyze_instruction(const IRInstruction* insn,
                           ConstPropEnvironment* current_state);
  void simplify_instruction(IRInstruction*& insn,
                            const ConstPropEnvironment& current_state);

  size_t num_branch_propagated() const { return m_branch_propagated; }
  size_t num_materialized_consts() const { return m_materialized_consts; }

  const InsnReplaceVector& insn_replacements() const {
    return m_insn_replacements;
  }

 private:
  void simplify_branch(IRInstruction*& inst,
                       const ConstPropEnvironment& current_state);

  using value_transform_t = std::function<boost::optional<int32_t>(int32_t)>;
  using wide_value_transform_t =
      std::function<boost::optional<int64_t>(int64_t)>;

  static boost::optional<int32_t> identity(int32_t v) { return v; }
  static boost::optional<int64_t> wide_identity(int64_t v) { return v; }

  void analyze_non_branch(
      const IRInstruction* inst,
      ConstPropEnvironment* current_state,
      bool is_wide,
      value_transform_t value_transform = identity,
      wide_value_transform_t wide_value_transform = wide_identity);

  void simplify_non_branch(
      IRInstruction* inst,
      const ConstPropEnvironment& current_state,
      bool is_wide);

  void propagate(DexMethod* method);

  InsnReplaceVector m_insn_replacements;
  size_t m_branch_propagated;
  size_t m_materialized_consts;
  const ConstPropConfig& m_config;
};

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
