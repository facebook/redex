/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <boost/dynamic_bitset.hpp>

class LocalDce {
 public:
  struct Stats {
    size_t dead_instruction_count{0};
    size_t unreachable_instruction_count{0};
  };

  /*
   * Eliminate dead code using a standard backward dataflow analysis for
   * liveness.  The algorithm is as follows:
   *
   * - Maintain a bitvector for each block representing the liveness for each
   *   register.  Function call results are represented by bit #num_regs.
   *
   * - Walk the blocks in postorder. Compute each block's output state by
   *   OR-ing the liveness of its successors
   *
   * - Walk each block's instructions in reverse to determine its input state.
   *   An instruction's input registers are live if (a) it has side effects, or
   *   (b) its output registers are live.
   *
   * - If the liveness of any block changes during a pass, repeat it.  Since
   *   anything live in one pass is guaranteed to be live in the next, this is
   *   guaranteed to reach a fixed point and terminate.  Visiting blocks in
   *   postorder guarantees a minimum number of passes.
   *
   * - Catch blocks are handled slightly differently; since any instruction
   *   inside a `try` region can jump to a catch block, we assume that any
   *   registers that are live-in to a catch block must be kept live throughout
   *   the `try` region.  (This is actually conservative, since only
   *   potentially-excepting instructions can jump to a catch.)
   */

  LocalDce(const std::unordered_set<DexMethodRef*>& pure_methods)
      : m_pure_methods(pure_methods) {}

  const Stats& get_stats() const { return m_stats; }

  void dce(IRCode*);

 private:
  const std::unordered_set<DexMethodRef*>& m_pure_methods;
  Stats m_stats;

  bool is_required(IRInstruction* inst,
                   const boost::dynamic_bitset<>& bliveness);
  bool is_pure(DexMethodRef* ref, DexMethod* meth);
};

class LocalDcePass : public Pass {
 public:
  LocalDcePass() : Pass("LocalDcePass") {}

  static void run(IRCode* code);

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  static std::unordered_set<DexMethodRef*> find_pure_methods();
};
