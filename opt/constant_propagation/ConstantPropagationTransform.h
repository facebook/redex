/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "IRCode.h"

namespace constant_propagation {

/**
 * Optimize the given code by removing dead branches and converting move
 * instructions to const instructions when the values are known.
 */
class Transform final {
 public:
  struct Stats {
    size_t branches_removed{0};
    size_t materialized_consts{0};
    Stats operator+(const Stats& that) const {
      Stats result;
      result.branches_removed = branches_removed + that.branches_removed;
      result.materialized_consts =
          materialized_consts + that.materialized_consts;
      return result;
    }
  };

  explicit Transform(const ConstPropConfig& config) : m_config(config) {}

  Stats apply(const intraprocedural::FixpointIterator&, IRCode*);

 private:
  /*
   * The simplify_* methods queue up their transformations in
   * m_insns_replacement. After they are done, the apply_changes() method
   * does the actual modification of the IRCode.
   */

  void simplify_instruction(IRInstruction*, const ConstantEnvironment&);

  void simplify_branch(IRInstruction*, const ConstantEnvironment&);

  void simplify_non_branch(IRInstruction*,
                           const ConstantEnvironment&,
                           bool is_wide);

  void apply_changes(IRCode*);

  const ConstPropConfig& m_config;
  std::vector<std::pair<IRInstruction*, IRInstruction*>> m_insn_replacements;
  Stats m_stats;
};

} // namespace constant_propagation
