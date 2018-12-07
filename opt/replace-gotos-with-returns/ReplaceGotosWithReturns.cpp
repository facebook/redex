/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimizer pass replaces gotos that eventually simply return
 * by return instructions.
 *
 * Return instructions tend to have a smaller encoding than goto instructions,
 * and tend to compress better due to less entropy (no offset).
 */

#include "ReplaceGotosWithReturns.h"

#include <algorithm>
#include <boost/pending/disjoint_sets.hpp>
#include <boost/property_map/property_map.hpp>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_GOTOS_REPLACED_WITH_RETURNS =
    "num_gotos_replaced_with_returns";

} // namespace

size_t ReplaceGotosWithReturnsPass::process_code(IRCode* code) {
  code->build_cfg(/* editable = true*/);
  auto& cfg = code->cfg();

  // Inline all blocks that just contain a single return instruction and are
  // reached via a goto edge; this may leave behind some unreachable blocks
  // which will get cleaned up via other cfg mechanism eventually.

  std::vector<cfg::Edge*> edges_to_delete;
  auto order = cfg.order();
  for (auto it = order.begin(); it != order.end(); ++it) {
    cfg::Block* b = *it;
    auto last_it = b->get_last_insn();
    if (last_it == b->end()) {
      continue;
    }
    auto first_it = b->get_first_insn();
    if (first_it != last_it) {
      continue;
    }
    IRInstruction* insn = last_it->insn;
    if (!is_return(insn->opcode())) {
      continue;
    }

    const auto& preds = b->preds();
    for (cfg::Edge* e : preds) {
      if (e->type() != cfg::EDGE_GOTO) {
        continue;
      }

      cfg::Block* src = e->src();
      if (it != order.begin() && *std::prev(it) == src) {
        TRACE(RGWR, 4, "Skipped a return\n");
        // don't put in a return instruction if we would just fall through
        // anyway, i.e. if linearization won't insert a goto here
        continue;
      }

      IRInstruction* cloned_insn = new IRInstruction(insn->opcode());
      if (insn->srcs_size()) {
        cloned_insn->set_src(0, insn->src(0));
      }
      auto cloned_mie = new MethodItemEntry(cloned_insn);
      src->get_entries().push_back(*cloned_mie);
      edges_to_delete.push_back(e);
    }
  }

  for (cfg::Edge* e : edges_to_delete) {
    cfg.delete_edge(e);
  }

  code->clear_cfg();
  return edges_to_delete.size();
}

void ReplaceGotosWithReturnsPass::run_pass(DexStoresVector& stores,
                                           ConfigFiles& /* unused */,
                                           PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t total_gotos_replaced = walk::parallel::reduce_methods<size_t>(
      scope,
      [](DexMethod* method) -> size_t {
        const auto code = method->get_code();
        if (!code) {
          return 0;
        }

        size_t gotos_replaced = ReplaceGotosWithReturnsPass::process_code(code);
        if (gotos_replaced) {
          TRACE(RGWR, 3, "Replaced %u gotos with returns in {%s}\n",
                gotos_replaced, SHOW(method));
        }
        return gotos_replaced;
      },
      [](size_t a, size_t b) { return a + b; });

  mgr.incr_metric(METRIC_GOTOS_REPLACED_WITH_RETURNS, total_gotos_replaced);
  TRACE(RGWR, 1, "Replaced %u gotos with returns.\n", total_gotos_replaced);
}

static ReplaceGotosWithReturnsPass s_pass;
