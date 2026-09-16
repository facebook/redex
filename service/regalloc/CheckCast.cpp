/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CheckCast.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "IRInstruction.h"
#include "Lazy.h"
#include "Liveness.h"
#include "Show.h"
#include "Timer.h"

namespace {

AccumulatingTimer s_timer("split_check_cast_result_live_ranges");

} // namespace

namespace regalloc {

/*
 * live_range::renumber_registers() assigns one symbolic register to all
 * definitions that reach a common source operand. This is required because the
 * joined use has only one register operand. If a successful check-cast result
 * and the pre-cast value retained through an exception handler reach the same
 * later use, they therefore become one symbolic register.
 *
 * The check-cast interference rule normally keeps its result separate from
 * handler-live values. For this merged web it instead tries to add a self-edge,
 * which Graph::add_edge() discards. Keeping the two definitions in separate
 * webs would not be sufficient: the joined use would still need a copy that
 * selects one value from either predecessor.
 *
 * Give the successful result a fresh register and copy it to the joined
 * register on the non-throwing continuation. This runs after renumbering by
 * design: renumbering first removes coincidental register reuse, so every
 * remaining split represents an actual joined web. The restored interference
 * edge prevents RegAlloc from coalescing the fresh result with the handler-live
 * value.
 */
size_t split_check_cast_result_live_ranges(cfg::ControlFlowGraph& cfg) {
  auto timer_scope = s_timer.scope();
  std::vector<cfg::Block*> check_cast_blocks;
  for (auto* block : cfg.blocks()) {
    if (cfg.get_succ_edges_of_type(block, cfg::EDGE_THROW).empty()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      if (mie.insn->opcode() == OPCODE_CHECK_CAST) {
        check_cast_blocks.push_back(block);
        break;
      }
    }
  }
  if (check_cast_blocks.empty()) {
    return 0;
  }

  Lazy<LivenessFixpointIterator> fixpoint_iter([&cfg] {
    // This may delete a synthetic exit block. The filtered block snapshot above
    // cannot contain that instruction-less block.
    cfg.calculate_exit_block();
    auto res = std::make_unique<LivenessFixpointIterator>(cfg);
    res->run({});
    return res;
  });

  cfg::CFGMutation mutation(cfg);
  size_t split_count{0};
  for (auto* block : check_cast_blocks) {
    const auto& throw_edges =
        cfg.get_succ_edges_of_type(block, cfg::EDGE_THROW);
    for (auto& mie : InstructionIterable(block)) {
      auto* check_cast = mie.insn;
      if (check_cast->opcode() != OPCODE_CHECK_CAST) {
        continue;
      }
      auto check_cast_it = block->to_cfg_instruction_iterator(mie);
      auto pseudo_result_it = cfg.move_result_of(check_cast_it);
      always_assert_log(!pseudo_result_it.is_end(),
                        "Missing pseudo-result for %s",
                        SHOW(check_cast));
      auto* pseudo_result = pseudo_result_it->insn;
      always_assert_log(pseudo_result->opcode() ==
                            IOPCODE_MOVE_RESULT_PSEUDO_OBJECT,
                        "Unexpected pseudo-result %s for %s",
                        SHOW(pseudo_result),
                        SHOW(check_cast));
      auto dest = pseudo_result->dest();
      if (dest == check_cast->src(0)) {
        continue;
      }
      auto is_live_in_throw_target = std::any_of(
          throw_edges.begin(), throw_edges.end(), [&](cfg::Edge* edge) {
            return fixpoint_iter->get_live_in_vars_at(edge->target())
                .elements()
                .contains(dest);
          });
      if (!is_live_in_throw_target) {
        continue;
      }
      auto temp = cfg.allocate_temp();
      pseudo_result->set_dest(temp);
      auto* move = new IRInstruction(OPCODE_MOVE_OBJECT);
      move->set_dest(dest);
      move->set_src(0, temp);
      mutation.insert_after(pseudo_result_it, {move});
      ++split_count;
    }
  }

  mutation.flush();
  return split_count;
}

} // namespace regalloc
