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
#include "ConstantPropagationWholeProgramState.h"
#include "IRCode.h"

namespace constant_propagation {

/**
 * Optimize the given code by:
 *   - removing dead branches
 *   - converting instructions to `const` when the values are known
 *   - removing field writes if they all write the same constant value
 */
class Transform final {
 public:
  struct Config {
    bool replace_moves_with_consts;
  };

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

  explicit Transform(const Config& config) : m_config(config) {}

  Stats apply(const intraprocedural::FixpointIterator&,
              const WholeProgramState&,
              IRCode*);

 private:
  /*
   * The methods in this class queue up their transformations. After they are
   * all done, the apply_changes() method does the actual modification of the
   * IRCode.
   */
  void apply_changes(IRCode*);

  void simplify_instruction(const ConstantEnvironment&,
                            const WholeProgramState& wps,
                            IRList::iterator);

  void replace_with_const(const ConstantEnvironment&, IRList::iterator);

  void eliminate_dead_branch(const intraprocedural::FixpointIterator&,
                             const ConstantEnvironment&,
                             cfg::Block*);

  const Config m_config;
  std::vector<std::pair<IRInstruction*, IRInstruction*>> m_replacements;
  std::vector<IRList::iterator> m_deletes;
  Stats m_stats;
};

} // namespace constant_propagation
