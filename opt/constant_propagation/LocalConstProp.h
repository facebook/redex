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

namespace constant_propagation_impl {

template <typename Integral>
bool get_constant_value(const ConstPropEnvironment& env,
                        int16_t reg,
                        Integral& result);

template <>
inline bool get_constant_value(const ConstPropEnvironment& env,
                               int16_t reg,
                               int64_t& result) {
  if (ConstPropEnvUtil::is_wide_constant(env, reg)) {
    result = ConstPropEnvUtil::get_wide(env, reg);
    return true;
  } else {
    return false;
  }
}

template <>
inline bool get_constant_value(const ConstPropEnvironment& env,
                               int16_t reg,
                               int32_t& result) {
  if (ConstPropEnvUtil::is_narrow_constant(env, reg)) {
    result = ConstPropEnvUtil::get_narrow(env, reg);
    return true;
  } else {
    return false;
  }
}

/*
 * Returns the value of inst->src(src_idx) if it exists and is a constant,
 * otherwise return the default value. This is a helper function to ease
 * handling of if-{eq,ne,lt,...}(z?) opcodes, i.e. branch opcodes that have
 * compare-to-zero flavors. XXX(jezng) ideally, we'll remove compare-to-zero
 * opcodes from our IR, then this wouldn't be necessary.
 */
template <typename Integral>
bool get_constant_value_at_src(const ConstPropEnvironment& env,
                               const IRInstruction* inst,
                               uint32_t src_idx,
                               Integral default_value,
                               Integral& result) {
  if (src_idx < inst->srcs_size()) {
    return get_constant_value(env, inst->src(src_idx), result);
  }
  result = default_value;
  return true;
}

} // constant_propagation_impl

// Must be IEEE 754
static_assert(std::numeric_limits<float>::is_iec559,
              "Can't propagate floats because IEEE 754 is not supported in "
              "this architecture");
static_assert(std::numeric_limits<double>::is_iec559,
              "Can't propagate doubles because IEEE 754 is not supported in "
              "this architecture");
