/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRCode.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "MethodOverrideGraph.h"

#include <boost/dynamic_bitset.hpp>

class DexMethod;
class DexMethodRef;

namespace cfg {
class Block;
class ControlFlowGraph;
class Edge;
} // namespace cfg

class LocalDce {
 public:
  struct Stats {
    size_t npe_instruction_count{0};
    size_t init_class_instructions_added{0};
    size_t dead_instruction_count{0};
    size_t unreachable_instruction_count{0};
    size_t aliased_new_instances{0};
    size_t normalized_new_instances{0};
    init_classes::Stats init_classes;

    Stats& operator+=(const Stats& that) {
      npe_instruction_count += that.npe_instruction_count;
      init_class_instructions_added += that.init_class_instructions_added;
      dead_instruction_count += that.dead_instruction_count;
      unreachable_instruction_count += that.unreachable_instruction_count;
      aliased_new_instances += that.aliased_new_instances;
      normalized_new_instances += that.normalized_new_instances;
      init_classes += that.init_classes;
      return *this;
    }
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

  explicit LocalDce(
      const init_classes::InitClassesWithSideEffects*
          init_classes_with_side_effects,
      const std::unordered_set<DexMethodRef*>& pure_methods,
      const method_override_graph::Graph* method_override_graph = nullptr,
      bool may_allocate_registers = false,
      bool ignore_pure_method_init_classes = false)
      : m_init_classes_with_side_effects(init_classes_with_side_effects),
        m_pure_methods(pure_methods),
        m_method_override_graph(method_override_graph),
        m_may_allocate_registers(may_allocate_registers),
        m_ignore_pure_method_init_classes(ignore_pure_method_init_classes) {}

  const Stats& get_stats() const { return m_stats; }

  void dce(IRCode*,
           bool normalize_new_instances = true,
           DexType* declaring_type = nullptr);
  void dce(cfg::ControlFlowGraph&,
           bool normalize_new_instances = true,
           DexType* declaring_type = nullptr);
  std::vector<std::pair<cfg::Block*, IRList::iterator>> get_dead_instructions(
      const cfg::ControlFlowGraph& cfg,
      const std::vector<cfg::Block*>& blocks,
      bool* any_init_class_insns);

 private:
  const init_classes::InitClassesWithSideEffects*
      m_init_classes_with_side_effects;
  const std::unordered_set<DexMethodRef*>& m_pure_methods;
  const method_override_graph::Graph* m_method_override_graph;
  const bool m_may_allocate_registers;
  const bool m_ignore_pure_method_init_classes;
  Stats m_stats;

  bool is_required(const cfg::ControlFlowGraph& cfg,
                   cfg::Block* b,
                   IRInstruction* inst,
                   const boost::dynamic_bitset<>& bliveness);
  bool assumenosideeffects(DexMethodRef* ref, DexMethod* meth);
  void normalize_new_instances(cfg::ControlFlowGraph& cfg);
  void prune_init_classes(cfg::ControlFlowGraph& cfg, DexType* declaring_type);
};
