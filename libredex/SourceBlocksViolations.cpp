/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocksViolations.h"

#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <sparta/S_Expression.h>

#include "CallGraph.h"
#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "Dominators.h"
#include "IRList.h"
#include "IROpcode.h"
#include "Macros.h"
#include "MethodOverrideGraph.h"
#include "ScopedCFG.h"
#include "ScopedMetrics.h"
#include "Show.h"
#include "ShowCFG.h"
#include "SourceBlockConsistencyCheck.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"

namespace source_blocks {

using namespace cfg;
using namespace sparta;

namespace {

bool is_less_than_for_any_value(
    const SourceBlock* lhs,
    const SourceBlock* rhs,
    uint32_t max_interaction = std::numeric_limits<uint32_t>::max(),
    bool ignore_undefined = false) {
  always_assert(lhs != nullptr && rhs != nullptr);
  auto limit =
      std::min(std::min(lhs->vals_size, rhs->vals_size), max_interaction);
  for (size_t i = 0; i != limit; ++i) {
    if (ignore_undefined && (lhs->get_at(i) == SourceBlock::Val::none() ||
                             rhs->get_at(i) == SourceBlock::Val::none())) {
      return false;
    }
    if (lhs->get_val(i).value_or(0) < rhs->get_val(i).value_or(0)) {
      return true;
    }
  }
  return false;
}

bool is_ghost_block(Block* block) {
  for (const auto& edge : block->preds()) {
    if (edge->type() == cfg::EDGE_GHOST) {
      return true;
    }
  }
  return false;
}

} // namespace

static void process_source_blocks_for_violations(SourceBlock* first_sb,
                                                 std::vector<bool>& hit_tracker,
                                                 bool update_hit_before_fix) {
  for (auto* sb = first_sb; sb != nullptr; sb = sb->next.get()) {
    for (size_t i = 0; i < sb->vals_size; i++) {
      always_assert(i < hit_tracker.size());
      bool is_hit = sb->get_val(i).value_or(0) > 0;

      if (update_hit_before_fix && is_hit) {
        hit_tracker[i] = true;
      }

      if (hit_tracker[i] && sb->get_val(i).value_or(0) <= 0) {
        sb->set_at(i, SourceBlock::Val(1, sb->get_appear100(i).value_or(1)));
      }

      if (!update_hit_before_fix && is_hit) {
        hit_tracker[i] = true;
      }
    }
  }
}

void fix_chain_violations(ControlFlowGraph* cfg) {
  impl::visit_in_order(
      cfg,
      [&](Block* cur) {
        uint32_t vals_size =
            source_blocks::get_first_source_block(cur)->vals_size;
        // Iterate over the source blocks in reverse order
        // If a source block is hit for an interaction, then all
        // source blocks preceding it must be hit for that interaction
        std::vector<bool> any_hit_rev(vals_size, false);
        for (auto mie_it = cur->rbegin(); mie_it != cur->rend(); mie_it++) {
          auto& mie = *mie_it;
          if (mie.type == MFLOW_SOURCE_BLOCK) {
            process_source_blocks_for_violations(
                mie.src_block.get(), any_hit_rev,
                /* update_hit_before_fix= */ false);
          }
        }
        // Iterate over the source blocks in forward order
        // If a source block is hit for an interaction, then all
        // source blocks after it must be hit for that interaction
        // unless it is separated by a throwing instruction
        std::vector<bool> any_hit_for(vals_size, false);
        for (auto& mie : *cur) {
          if (mie.type == MFLOW_OPCODE &&
              opcode::can_throw(mie.insn->opcode())) {
            std::fill(any_hit_for.begin(), any_hit_for.end(), false);
          } else if (mie.type == MFLOW_SOURCE_BLOCK) {
            process_source_blocks_for_violations(
                mie.src_block.get(), any_hit_for,
                /* update_hit_before_fix= */ true);
          }
        }
      },
      [&](Block* /* cur */, const Edge* /* e */) {}, [&](Block* /* cur */) {});
}

void fix_idom_violation(
    Block* cur,
    uint32_t vals_index,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  bool first_source_block_changed = true;
  if (source_blocks::get_first_source_block(cur)
          ->get_val(vals_index)
          .value_or(0) > 0) {
    first_source_block_changed = false;
  }
  foreach_source_block(cur, [&](auto& sb) {
    if (sb->get_val(vals_index).value_or(0) <= 0) {
      sb->set_at(
          vals_index,
          SourceBlock::Val(1, sb->get_appear100(vals_index).value_or(1)));
    }
  });
  auto* idom = dom.get_idom(cur);
  if (first_source_block_changed && idom != cur) {
    fix_idom_violation(idom, vals_index, dom);
  }
}

void fix_idom_violations(ControlFlowGraph* cfg) {
  dominators::SimpleFastDominators<cfg::GraphInterface> dom{*cfg};
  impl::visit_in_order(
      cfg,
      [&](Block* cur) {
        auto* first_sb = source_blocks::get_first_source_block(cur);
        auto* idom = dom.get_idom(cur);
        if (idom != cur) {
          uint32_t vals_size = first_sb->vals_size;
          for (uint32_t i = 0; i < vals_size; i++) {
            if (first_sb->get_val(i).value_or(0) > 0) {
              fix_idom_violation(idom, i, dom);
            }
          }
        }
      },
      [&](Block* /* cur */, const Edge* /* e */) {}, [&](Block* /* cur */) {});
}

void fix_hot_method_cold_entry_violations(ControlFlowGraph* cfg) {
  auto* entry_block = cfg->entry_block();
  if (entry_block == nullptr) {
    return;
  }
  auto* sb = get_first_source_block(entry_block);
  if (sb == nullptr) {
    return;
  }
  uint32_t vals_size = sb->vals_size;
  for (uint32_t i = 0; i < vals_size; i++) {
    if (sb->get_val(i).value_or(0) <= 0 &&
        sb->get_appear100(i).value_or(0) > 0) {
      sb->set_at(i, SourceBlock::Val(1, sb->get_appear100(i).value_or(1)));
    }
  };
}

namespace {

size_t count_blocks(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return 1;
}
size_t count_block_has_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return source_blocks::has_source_blocks(b) ? 1 : 0;
}
size_t count_block_no_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return source_blocks::has_source_blocks(b) ? 0 : 1;
}
size_t count_block_has_incomplete_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  auto* sb = get_first_source_block(b);
  if (sb == nullptr) {
    return 0;
  }
  return static_cast<size_t>(
      sb->foreach_val_early([&](const auto& val) { return !val; }));
}
size_t count_all_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  size_t ret{0};
  source_blocks::foreach_source_block(
      b, [&](auto* sb ATTRIBUTE_UNUSED) { ++ret; });
  return ret;
}
size_t count_throw_delineated_no_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  size_t num_violations = 0;
  bool had_sb = false;
  for (auto it = b->begin(); it != b->end(); ++it) {
    if (it->type == MFLOW_SOURCE_BLOCK) {
      had_sb = true;
    }
    if (it->type == MFLOW_OPCODE && opcode::can_throw(it->insn->opcode()) &&
        it->insn->opcode() != OPCODE_THROW) {
      if (!had_sb) {
        num_violations++;
      }
      had_sb = false;
    }
  }
  if (!had_sb && !b->empty()) {
    num_violations++;
  }
  return num_violations;
}

struct ViolationsAndPotentialViolations {
  size_t violations{0};
  size_t possible_violations{0};
};

// TODO: Per-interaction stats.

ViolationsAndPotentialViolations hot_immediate_dom_not_hot_impl(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dominators,
    bool ignore_undefined) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);

  auto* immediate_dominator = dominators.get_idom(block);
  if (immediate_dominator == nullptr) {
    return {0, 0};
  }
  auto* first_sb_immediate_dominator =
      source_blocks::get_first_source_block(immediate_dominator);
  if ((first_sb_current_b != nullptr) &&
      (first_sb_immediate_dominator != nullptr) &&
      is_less_than_for_any_value(first_sb_immediate_dominator,
                                 first_sb_current_b,
                                 std::numeric_limits<uint32_t>::max(),
                                 ignore_undefined)) {
    return {1, 1};
  } else {
    return {0, 1};
  }
}

ViolationsAndPotentialViolations hot_immediate_dom_not_hot(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dominators,
    bool ignore_undefined) {
  return hot_immediate_dom_not_hot_impl(block, dominators, ignore_undefined);
}

ViolationsAndPotentialViolations hot_no_hot_pred(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool ignore_undefined) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);
  if (!has_source_block_positive_val(first_sb_current_b)) {
    return {0, 0};
  }

  if (block->preds().empty()) {
    return {0, 0};
  }

  std::vector<float> summed_values(first_sb_current_b->vals_size);
  for (auto* predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (first_sb_pred != nullptr) {
      for (uint32_t i = 0; i < std::min(first_sb_current_b->vals_size,
                                        first_sb_pred->vals_size);
           i++) {
        if (ignore_undefined &&
            first_sb_pred->get_at(i) == SourceBlock::Val::none()) {
          return {0, 1};
        }
        summed_values[i] += first_sb_pred->get_val(i).value_or(0);
      }
    }
  }
  for (uint32_t i = 0; i < first_sb_current_b->vals_size; i++) {
    if (ignore_undefined &&
        first_sb_current_b->get_at(i) == SourceBlock::Val::none()) {
      return {0, 1};
    }
    if (summed_values[i] < first_sb_current_b->get_val(i).value_or(0)) {
      return {1, 1};
    }
  }
  return {0, 1};
}

ViolationsAndPotentialViolations hot_all_children_cold(Block* block,
                                                       bool ignore_undefined) {
  auto* last_sb_before_throw =
      source_blocks::get_last_source_block_if_after_throw(block);

  if ((last_sb_before_throw == nullptr) ||
      !has_source_block_positive_val(last_sb_before_throw)) {
    return {0, 0};
  }

  bool has_successor = false;
  std::vector<float> summed_values(last_sb_before_throw->vals_size);
  for (auto* successor : block->succs()) {
    auto* first_sb_succ =
        source_blocks::get_first_source_block(successor->target());
    has_successor = true;
    if (first_sb_succ != nullptr) {
      for (uint32_t i = 0; i < std::min(last_sb_before_throw->vals_size,
                                        first_sb_succ->vals_size);
           i++) {
        if (ignore_undefined &&
            first_sb_succ->get_at(i) == SourceBlock::Val::none()) {
          return {0, 1};
        }
        summed_values[i] += first_sb_succ->get_val(i).value_or(0);
      }
    }
  }
  if (!has_successor) {
    return {0, 0};
  }
  for (uint32_t i = 0; i < last_sb_before_throw->vals_size; i++) {
    if (ignore_undefined &&
        last_sb_before_throw->get_at(i) == SourceBlock::Val::none()) {
      return {0, 1};
    }
    // This means that for this current hot block (with respect to the last
    // source block of the hot block), the sum of the hit values of its children
    // must be greater or equal to its hit values
    if (summed_values[i] < last_sb_before_throw->get_val(i).value_or(0)) {
      return {1, 1};
    }
  }
  return {0, 1};
}

using SourceBlockHotInvokeMap =
    ConcurrentMap<const DexMethod*, UnorderedSet<IRInstruction*>>;

size_t hot_callee_all_cold_callers(
    call_graph::NodeId node, SourceBlockHotInvokeMap& src_block_invoke_map) {
  // Ignore Ghost Nodes
  if (node->is_entry() || node->is_exit()) {
    return 0;
  }

  auto* callee_method = const_cast<DexMethod*>(node->method());
  if (callee_method == nullptr || callee_method->get_code() == nullptr) {
    return 0;
  }

  ScopedCFG callee_cfg(callee_method->get_code());

  // Cold Callees do not count
  auto* callee_entry_block = get_first_source_block(callee_cfg->entry_block());
  if ((callee_entry_block == nullptr) || callee_entry_block->vals_size == 0 ||
      callee_entry_block->get_val(0).value_or(0) == 0) {
    return 0;
  }

  for (const auto* caller_edge : node->callers()) {
    const auto* caller = caller_edge->caller();
    // If a node is connected to the ghost entry node, we should not count it
    // as a violation because we can treat a ghost node's transition to its
    // successors as hot
    if (caller->is_entry()) {
      return 0;
    }

    auto* caller_method = const_cast<DexMethod*>(caller->method());
    if (caller_method == nullptr || caller_method->get_code() == nullptr) {
      continue;
    }

    auto* invoke_insn = caller_edge->invoke_insn();
    // TODO(T229471397): With multiple-callee graphs, there might be more
    // accurate way to check which specific method is calling the callee
    // (method override check)

    if (src_block_invoke_map.count(caller_method) == 0) {
      continue;
    }
    const auto& insn_map = src_block_invoke_map.at_unsafe(caller_method);
    const auto& source_block_bools_before_invoke = insn_map.find(invoke_insn);
    if (source_block_bools_before_invoke != insn_map.end()) {
      return 0;
    }
  }
  return 1;
}

// Helper transforms for violation counting
namespace {
constexpr auto identity_transform = [](auto val) { return val; };
constexpr auto binary_transform = [](auto val) { return val > 0 ? 1 : 0; };
} // namespace

template <typename Fn>
ViolationsAndPotentialViolations chain_hot_violations_tmpl(Block* block,
                                                           const Fn& fn) {
  size_t sum{0};
  size_t possible_violations{0};
  for (auto& mie : *block) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }

    for (auto* sb = mie.src_block.get(); sb->next != nullptr;
         sb = sb->next.get()) {
      // Check that each interaction has at least as high a hit value as the
      // next SourceBlock.
      auto* next = sb->next.get();
      size_t local_sum{0};
      size_t local_possible_violations{0};
      for (size_t i = 0; i != sb->vals_size; ++i) {
        auto sb_val = sb->get_val(i);
        auto next_val = next->get_val(i);
        if (sb_val) {
          if (next_val && *sb_val < *next_val) {
            ++local_sum;
          }
        } else if (next_val) {
          ++local_sum;
        }
        ++local_possible_violations;
      }
      sum += fn(local_sum);
      possible_violations += fn(local_possible_violations);
    }
  }

  return {sum, possible_violations};
}

template <typename Fn>
ViolationsAndPotentialViolations hot_method_cold_entry_violations_tmpl(
    Block* block, const Fn& fn) {
  size_t sum{0};
  size_t possible_violations{0};
  if (block->preds().empty()) {
    auto* sb = get_first_source_block(block);
    if (sb != nullptr) {
      sb->foreach_val([&sum, &possible_violations](const auto& val) {
        if (val && val->appear100 != 0 && val->val == 0) {
          sum++;
        }
        possible_violations++;
      });
    }
  }
  sum = fn(sum);
  possible_violations = fn(possible_violations);
  return {sum, possible_violations};
}

ViolationsAndPotentialViolations chain_hot_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool /*ignore_undefined*/) {
  return chain_hot_violations_tmpl(block, identity_transform);
}

ViolationsAndPotentialViolations chain_hot_one_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool /*ignore_undefined*/) {
  return chain_hot_violations_tmpl(block, binary_transform);
}

ViolationsAndPotentialViolations hot_method_cold_entry_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool /*ignore_undefined*/) {
  return hot_method_cold_entry_violations_tmpl(block, identity_transform);
}

ViolationsAndPotentialViolations hot_method_cold_entry_block_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool /*ignore_undefined*/) {
  return hot_method_cold_entry_violations_tmpl(block, binary_transform);
}

ViolationsAndPotentialViolations hot_all_children_cold_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool ignore_undefined) {
  return hot_all_children_cold(block, ignore_undefined);
};

struct ChainAndDomState {
  const SourceBlock* last{nullptr};
  cfg::Block* dom_block{nullptr};
  size_t violations{0};
  size_t possible_violations{0};
};

template <uint32_t kMaxInteraction>
void chain_and_dom_update(
    cfg::Block* block,
    const SourceBlock* sb,
    bool first_in_block,
    bool prev_insn_can_throw,
    ChainAndDomState& state,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom,
    bool ignore_undefined) {
  if (first_in_block) {
    state.last = nullptr;
    for (auto* b = dom.get_idom(block); state.last == nullptr && b != nullptr;
         b = dom.get_idom(b)) {
      if (b == block) {
        state.dom_block = nullptr;
        break;
      }
      state.last = get_last_source_block(b);
      if (b == b->cfg().entry_block()) {
        state.dom_block = b;
        break;
      }
    }
  } else {
    state.dom_block = nullptr;
  }

  if (state.last != nullptr) {
    bool cold_precedes_hot = is_less_than_for_any_value(
        state.last, sb, kMaxInteraction, ignore_undefined);
    bool hot_precedes_cold =
        is_less_than_for_any_value(sb, state.last, kMaxInteraction,
                                   ignore_undefined) &&
        !first_in_block && !prev_insn_can_throw;
    if (cold_precedes_hot || hot_precedes_cold) {
      state.violations++;
    }
    state.possible_violations++;
  }

  state.last = sb;
}

template <uint32_t kMaxInteraction>
ViolationsAndPotentialViolations chain_and_dom_violations_impl(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom,
    bool ignore_undefined) {
  ChainAndDomState state{};
  bool first = true;
  // True if any instruction that we've encountered since the last source
  // block can throw
  bool prev_insn_can_throw = false;
  for (const auto& mie : *block) {
    switch (mie.type) {
    case MFLOW_OPCODE:
      prev_insn_can_throw =
          prev_insn_can_throw || opcode::can_throw(mie.insn->opcode());
      break;
    case MFLOW_SOURCE_BLOCK:
      for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
        chain_and_dom_update<kMaxInteraction>(block, sb, first,
                                              prev_insn_can_throw, state, dom,
                                              ignore_undefined);
        first = false;
        prev_insn_can_throw = false;
      }
      break;
    default:
      break;
    }
  }

  return {state.violations, state.possible_violations};
}

ViolationsAndPotentialViolations chain_and_dom_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom,
    bool ignore_undefined) {
  return chain_and_dom_violations_impl<std::numeric_limits<uint32_t>::max()>(
      block, dom, ignore_undefined);
}

ViolationsAndPotentialViolations chain_and_dom_violations_coldstart(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom,
    bool ignore_undefined) {
  return chain_and_dom_violations_impl<1>(block, dom, ignore_undefined);
}

// Ugly but necessary for constexpr below.
using CounterFnPtr = size_t (*)(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&);
using ViolationCounterFnPtr = ViolationsAndPotentialViolations (*)(
    Block*,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&,
    bool ignore_undefined);

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 6> gCounters = {
    {{"~blocks~count", &count_blocks},
     {"~blocks~with~source~blocks", &count_block_has_sbs},
     {"~blocks~without~source~blocks", &count_block_no_sbs},
     {"~blocks~with~incomplete-source~blocks", &count_block_has_incomplete_sbs},
     {"~assessment~source~blocks~total", &count_all_sbs},
     {"~throw~delineated~blocks~without~source~blocks",
      &count_throw_delineated_no_sbs}}};

constexpr std::array<std::pair<std::string_view, ViolationCounterFnPtr>, 7>
    gViolationCounters = {
        {{"~flow~violation~in~chain", &chain_hot_violations},
         {"~flow~violation~in~chain~one", &chain_hot_one_violations},
         {"~flow~violation~chain~and~dom", &chain_and_dom_violations},
         {"~flow~violation~chain~and~dom.cold_start",
          &chain_and_dom_violations_coldstart},
         {"~flow~violation~seen~method~cold~entry",
          &hot_method_cold_entry_violations},
         {"~flow~violation~seen~method~cold~entry~blocks",
          &hot_method_cold_entry_block_violations},
         {"~flow~violation~hot~all~children~cold",
          &hot_all_children_cold_violations}}};

constexpr std::array<std::pair<std::string_view, ViolationCounterFnPtr>, 2>
    gViolationCountersNonEntry = {{
        {"~flow~violation~idom", &hot_immediate_dom_not_hot},
        {"~flow~violation~direct~predecessors", &hot_no_hot_pred},
    }};

struct SourceBlocksStats {
  size_t methods_with_sbs{0};

  struct Data {
    size_t count{0};
    size_t possible{0};
    size_t methods{0};
    size_t min{std::numeric_limits<size_t>::max()};
    size_t max{0};
    struct Method {
      const DexMethod* method;
      size_t num_opcodes;
    };
    std::optional<Method> min_method;
    std::optional<Method> max_method;

    Data& operator+=(const Data& rhs) {
      count += rhs.count;
      possible += rhs.possible;
      methods += rhs.methods;

      min = std::min(min, rhs.min);
      max = std::max(max, rhs.max);

      auto set_min_max = [](auto& lhs, auto& rhs, auto fn) {
        if (!rhs) {
          return;
        }
        if (!lhs) {
          lhs = rhs;
          return;
        }
        auto op = fn(lhs->num_opcodes, rhs->num_opcodes);
        if (op == rhs->num_opcodes &&
            (op != lhs->num_opcodes ||
             compare_dexmethods(rhs->method, lhs->method))) {
          lhs = rhs;
        }
      };

      set_min_max(min_method, rhs.min_method,
                  [](auto lhs, auto rhs) { return std::min(lhs, rhs); });
      set_min_max(max_method, rhs.max_method,
                  [](auto lhs, auto rhs) { return std::max(lhs, rhs); });

      return *this;
    }

    void fill_derived(const DexMethod* m) {
      methods = count > 0 ? 1 : 0;
      min = max = count;

      if (count != 0) {
        size_t num_opcodes = m->get_code()->count_opcodes();
        min_method = max_method = (Method){m, num_opcodes};
      }
    }
  };

  std::array<Data, gCounters.size()> global{};
  std::array<Data, gViolationCounters.size()> global_violations{};
  std::array<Data, gViolationCountersNonEntry.size()> non_entry_violations{};

  SourceBlocksStats& operator+=(const SourceBlocksStats& that) {
    methods_with_sbs += that.methods_with_sbs;

    for (size_t i = 0; i != global.size(); ++i) {
      global[i] += that.global[i];
    }

    for (size_t i = 0; i != global_violations.size(); ++i) {
      global_violations[i] += that.global_violations[i];
    }

    for (size_t i = 0; i != non_entry_violations.size(); ++i) {
      non_entry_violations[i] += that.non_entry_violations[i];
    }

    return *this;
  }

  void fill_derived(const DexMethod* m) {
    static_assert(gCounters[1].first == "~blocks~with~source~blocks");
    methods_with_sbs = global[1].count > 0 ? 1 : 0;

    for (auto& data : global) {
      data.fill_derived(m);
    }

    for (auto& data : global_violations) {
      data.fill_derived(m);
    }

    for (auto& data : non_entry_violations) {
      data.fill_derived(m);
    }
  }
};

} // namespace

void track_source_block_coverage(ScopedMetrics& sm,
                                 const DexStoresVector& stores) {
  Timer opt_timer("Calculate SourceBlock Coverage");
  auto stats = walk::parallel::methods<SourceBlocksStats>(
      build_class_scope(stores), [](DexMethod* m) -> SourceBlocksStats {
        SourceBlocksStats ret;
        auto* code = m->get_code();
        if (code == nullptr) {
          return ret;
        }
        code->build_cfg();
        auto& cfg = code->cfg();
        auto dominators =
            dominators::SimpleFastDominators<cfg::GraphInterface>(cfg);

        for (auto* block : cfg.blocks()) {
          for (size_t i = 0; i != gCounters.size(); ++i) {
            ret.global[i].count += (*gCounters[i].second)(block, dominators);
          }
          for (size_t i = 0; i != gViolationCounters.size(); ++i) {
            auto [violations, possible_violations] =
                (*gViolationCounters[i].second)(block, dominators, false);
            ret.global_violations[i].count += violations;
            ret.global_violations[i].possible += possible_violations;
          }
          if (block != cfg.entry_block()) {
            for (size_t i = 0; i != gViolationCountersNonEntry.size(); ++i) {
              auto [violations, possible_violations] =
                  (*gViolationCountersNonEntry[i].second)(block, dominators,
                                                          false);
              ret.non_entry_violations[i].count += violations;
              ret.non_entry_violations[i].possible += possible_violations;
            }
          }
        }

        code->clear_cfg();

        ret.fill_derived(m);

        return ret;
      });

  size_t consistency_check_violations =
      get_sbcc().is_initialized() ? get_sbcc().run(build_class_scope(stores))
                                  : 0;

  sm.set_metric("~consistency~check~violations", consistency_check_violations);

  sm.set_metric("~assessment~methods~with~sbs", stats.methods_with_sbs);

  auto data_metric = [&sm](const std::string_view& name, const auto& data,
                           bool violation) {
    sm.set_metric(name, data.count);

    auto scope = sm.scope(std::string(name));
    sm.set_metric("methods", data.methods);
    sm.set_metric("min", data.min);
    sm.set_metric("max", data.max);
    if (violation) {
      sm.set_metric("possible", data.possible);
    }

    auto min_max = [&](const auto& m, const char* name) {
      if (m) {
        auto min_max_scope = sm.scope(name);
        sm.set_metric(show_deobfuscated(m->method), m->num_opcodes);
      }
    };
    min_max(data.min_method, "min_method");
    min_max(data.max_method, "max_method");
  };
  for (size_t i = 0; i != gCounters.size(); ++i) {
    data_metric(gCounters[i].first, stats.global[i], false);
  }
  for (size_t i = 0; i != gViolationCounters.size(); ++i) {
    data_metric(gViolationCounters[i].first, stats.global_violations[i], true);
  }
  for (size_t i = 0; i != gViolationCountersNonEntry.size(); ++i) {
    data_metric(gViolationCountersNonEntry[i].first,
                stats.non_entry_violations[i], true);
  }
}

size_t compute_method_violations(const call_graph::Graph& call_graph,
                                 const Scope& scope) {
  size_t count{0};

  SourceBlockHotInvokeMap src_block_hot_invoke_map;
  // Builds a graph of method -> invoke insn -> vector<bool> each bool
  // representing if the coldstart interaction of the source block right
  // before that invoke is hot or not
  walk::parallel::methods(scope, [&](DexMethod* current_method) {
    if (current_method == nullptr || current_method->get_code() == nullptr) {
      return;
    }

    ScopedCFG cfg(current_method->get_code());
    cfg->remove_unreachable_blocks();

    IRInstruction* current_invoke_insn = nullptr;
    UnorderedSet<IRInstruction*> hot_set;
    for (auto* block : cfg->blocks()) {
      for (auto it = block->rbegin(); it != block->rend(); ++it) {
        if (it->type == MFLOW_OPCODE) {
          auto* instruction = it->insn;
          if (opcode::is_an_invoke(instruction->opcode())) {
            current_invoke_insn = instruction;
          }
        } else if (it->type == MFLOW_SOURCE_BLOCK) {
          if (current_invoke_insn != nullptr) {
            SourceBlock* sb = it->src_block.get();
            if ((sb != nullptr) && sb->vals_size == 0) {
              continue;
            }
            bool is_hot = (sb != nullptr) && sb->get_val(0).value_or(0) > 0;
            if (is_hot) {
              hot_set.insert(current_invoke_insn);
            }
            current_invoke_insn = nullptr;
          }
        }
      }
    }
    if (!hot_set.empty()) {
      src_block_hot_invoke_map.update(
          current_method, [&](const DexMethod*, auto& method_hot_set, bool) {
            method_hot_set = std::move(hot_set);
          });
    }
  });

  call_graph.visit_by_levels([&](call_graph::NodeId node) {
    count += hot_callee_all_cold_callers(node, src_block_hot_invoke_map);
  });

  return count;
}

struct ViolationsHelper::ViolationsHelperImpl {
  size_t top_n;
  UnorderedMap<DexMethod*, size_t> violations_start;
  size_t method_violations{0};
  std::vector<std::string> print;
  Scope scope;
  bool processed{false};
  bool track_intermethod_violations{false};
  bool print_all_violations{false};
  bool ignore_undefined{false};

  using Violation = ViolationsHelper::Violation;
  const Violation v;

  struct MethodDelta {
    DexMethod* method;
    size_t violations_delta;
    size_t method_size;

    MethodDelta(DexMethod* p1, size_t p2, size_t p3)
        : method(p1), violations_delta(p2), method_size(p3) {}

    // Comparison operator for sorting by proportional violations (descending)
    bool operator<(const MethodDelta& other) const {
      double this_proportional_violations =
          (double)violations_delta / (double)method_size;
      double other_proportional_violations =
          (double)other.violations_delta / (double)other.method_size;
      if (this_proportional_violations > other_proportional_violations) {
        return true;
      }
      if (this_proportional_violations < other_proportional_violations) {
        return false;
      }

      if (violations_delta > other.violations_delta) {
        return true;
      }
      if (violations_delta < other.violations_delta) {
        return false;
      }

      if (method_size < other.method_size) {
        return true;
      }
      if (method_size > other.method_size) {
        return false;
      }

      return compare_dexmethods(method, other.method);
    }
  };

  ViolationsHelperImpl(Violation v,
                       const Scope& scope,
                       size_t top_n,
                       std::vector<std::string> to_vis,
                       bool track_intermethod_violations,
                       bool print_all_violations,
                       bool ignore_undefined)
      : top_n(top_n),
        print(std::move(to_vis)),
        scope(scope),
        track_intermethod_violations(track_intermethod_violations),
        print_all_violations(print_all_violations),
        ignore_undefined(ignore_undefined),
        v(v) {
    {
      std::mutex lock;
      walk::parallel::methods(scope,
                              [this, &lock, v, ignore_undefined](DexMethod* m) {
                                if (m->get_code() == nullptr) {
                                  return;
                                }
                                cfg::ScopedCFG cfg(m->get_code());
                                auto val = compute(v, *cfg, ignore_undefined);
                                {
                                  std::unique_lock<std::mutex> ulock{lock};
                                  violations_start[m] = val;
                                }
                              });
    }

    if (track_intermethod_violations) {
      auto method_override_graph = method_override_graph::build_graph(scope);
      auto call_graph = std::make_unique<call_graph::Graph>(
          call_graph::single_callee_graph(*method_override_graph, scope));

      auto val = compute_method_violations(*call_graph, scope);
      method_violations = val;
    }

    print_all();
  }

  static size_t compute(Violation v,
                        cfg::ControlFlowGraph& cfg,
                        bool ignore_undefined) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot:
      return hot_immediate_dom_not_hot_cfg(cfg, ignore_undefined);
    case Violation::kChainAndDom:
      return chain_and_dom_violations_cfg(cfg, ignore_undefined);
    case Violation::kUncoveredSourceBlocks:
      return uncovered_source_blocks_violations_cfg(cfg);
    case Violation::kHotMethodColdEntry:
      return hot_method_cold_entry_violations_cfg(cfg, ignore_undefined);
    case Violation::kHotNoHotPred:
      return hot_no_hot_pred_cfg(cfg, ignore_undefined);
    case Violation::KHotAllChildrenCold:
      return hot_all_children_cold_cfg(cfg, ignore_undefined);
    case Violation::kUncoveredThrowDelineatedBlocks:
      return uncovered_throw_delineated_blocks_violations_cfg(cfg);
    case Violation::ViolationSize:
      not_reached();
    }
    not_reached();
  }

  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~ViolationsHelperImpl() { process(nullptr); }

  void silence() { processed = true; }
  void process(ScopedMetrics* sm) {
    if (processed) {
      return;
    }
    processed = true;

    std::atomic<size_t> change_sum{0};
    long long method_violation_change_sum{0};

    {
      std::mutex lock;
      std::vector<MethodDelta> top_changes;

      workqueue_run<std::pair<DexMethod*, size_t>>(
          [&](const std::pair<DexMethod*, size_t>& p) {
            auto* m = p.first;
            if (m->get_code() == nullptr) {
              return;
            }
            cfg::ScopedCFG cfg(m->get_code());
            auto val = compute(v, *cfg, ignore_undefined);
            if (val <= p.second) {
              return;
            }
            change_sum.fetch_add(val - p.second);

            auto m_delta = val - p.second;
            size_t s = m->get_code()->sum_opcode_sizes();
            std::unique_lock<std::mutex> ulock{lock};
            if (top_changes.size() < top_n) {
              top_changes.emplace_back(m, m_delta, s);
              return;
            }
            MethodDelta m_t{m, m_delta, s};
            if (m_t < top_changes.back()) {
              top_changes.back() = m_t;
              std::sort(top_changes.begin(), top_changes.end());
            }
          },
          violations_start);

      struct MaybeMetrics {
        ScopedMetrics* root{nullptr};
        std::optional<ScopedMetrics::Scope> scope;
        explicit MaybeMetrics(ScopedMetrics* root) : root(root) {}
        explicit MaybeMetrics(ScopedMetrics* root, ScopedMetrics::Scope sc)
            : root(root), scope(std::move(sc)) {}
        void set_metric(const std::string_view& key, int64_t value) {
          if (root != nullptr) {
            root->set_metric(key, value);
          }
        }
        MaybeMetrics sub_scope(std::string key) {
          if (root == nullptr) {
            return MaybeMetrics(nullptr);
          }
          return MaybeMetrics(root, root->scope(std::move(key)));
        }
      };
      MaybeMetrics mm(sm);
      auto mm_top_changes = mm.sub_scope("top_changes");
      for (size_t i = 0; i != top_changes.size(); ++i) {
        auto& t = top_changes[i];
        TRACE(MMINL, 0, "%s (size %zu): +%zu", SHOW(t.method), t.method_size,
              t.violations_delta);
        auto mm_top_changes_i = mm_top_changes.sub_scope(std::to_string(i));
        {
          auto mm_top_changes_i_size = mm_top_changes_i.sub_scope("size");
          mm_top_changes_i_size.set_metric(show(t.method),
                                           static_cast<int64_t>(t.method_size));
        }
        {
          auto mm_top_changes_i_size = mm_top_changes_i.sub_scope("delta");
          mm_top_changes_i_size.set_metric(
              show(t.method), static_cast<int64_t>(t.violations_delta));
        }
      }
    }

    print_all();

    TRACE(MMINL, 0, "Introduced %zu violations.", change_sum.load());
    if (track_intermethod_violations) {
      auto method_override_graph = method_override_graph::build_graph(scope);
      auto call_graph = std::make_unique<call_graph::Graph>(
          call_graph::single_callee_graph(*method_override_graph, scope));
      auto val = compute_method_violations(*call_graph, scope);
      method_violation_change_sum =
          static_cast<long long>(val - method_violations);
      TRACE(MMINL, 0, "Introduced %lld inter-method violations.",
            method_violation_change_sum);
    }
    if (sm != nullptr) {
      sm->set_metric("new_violations", change_sum.load());
      sm->set_metric("new_method_violations", method_violation_change_sum);
    }
  }

  static size_t hot_immediate_dom_not_hot_cfg(cfg::ControlFlowGraph& cfg,
                                              bool ignore_undefined) {
    size_t sum{0};

    // Some passes may leave around unreachable blocks which the fast-dom
    // does not deal well with.
    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += hot_immediate_dom_not_hot(b, dom, ignore_undefined).violations;
    }
    return sum;
  }

  static size_t chain_and_dom_violations_cfg(cfg::ControlFlowGraph& cfg,
                                             bool ignore_undefined) {
    size_t sum{0};

    // Some passes may leave around unreachable blocks which the fast-dom
    // does not deal well with.
    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += chain_and_dom_violations(b, dom, ignore_undefined).violations;
    }
    return sum;
  }

  static size_t uncovered_throw_delineated_blocks_violations_cfg(
      cfg::ControlFlowGraph& cfg) {
    size_t num_violations = 0;
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};
    for (auto* cur : cfg.blocks()) {
      num_violations += count_throw_delineated_no_sbs(cur, dom);
    }
    return num_violations;
  }

  static size_t uncovered_source_blocks_violations_cfg(
      cfg::ControlFlowGraph& cfg) {
    size_t sum{0};
    for (auto* b : cfg.blocks()) {
      if (is_ghost_block(b)) {
        // Do not count ghost blocks in this violation
        continue;
      }
      auto* sb = get_first_source_block(b);
      if (sb == nullptr) {
        sum++;
      }
    }
    return sum;
  }

  static size_t hot_method_cold_entry_violations_cfg(cfg::ControlFlowGraph& cfg,
                                                     bool ignore_undefined) {
    size_t sum{0};
    auto* entry_block = cfg.entry_block();
    if (entry_block == nullptr) {
      return 0;
    }
    auto* sb = get_first_source_block(entry_block);
    if (sb == nullptr) {
      return 0;
    }
    sb->foreach_val([&sum, ignore_undefined](const auto& val) {
      if (!ignore_undefined and !val) {
        sum++;
      }
      if (val && val->appear100 != 0 && val->val == 0) {
        sum++;
      }
    });
    return sum;
  }

  static size_t hot_no_hot_pred_cfg(cfg::ControlFlowGraph& cfg,
                                    bool ignore_undefined) {
    size_t sum{0};

    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += hot_no_hot_pred(b, dom, ignore_undefined).violations;
    }
    return sum;
  }

  static size_t hot_all_children_cold_cfg(cfg::ControlFlowGraph& cfg,
                                          bool ignore_undefined) {
    size_t sum{0};

    cfg.remove_unreachable_blocks();

    for (auto* b : cfg.blocks()) {
      sum += hot_all_children_cold(b, ignore_undefined).violations;
    }
    return sum;
  }

  void print_all() const {
    for (const auto& m_str : print) {
      auto* m = DexMethod::get_method(m_str);
      if (m != nullptr) {
        redex_assert(m != nullptr && m->is_def());
        auto* m_def = m->as_def();
        log_cfg_violations(v, m_def, ignore_undefined);
        if (print_all_violations) {
          print_cfg_with_all_violating_blocks(m_def, ignore_undefined);
        }
      }
    }

    walk::methods(scope, [this](DexMethod* m) {
      auto* code = m->get_code();
      if (code == nullptr) {
        return;
      }
      cfg::ScopedCFG cfg(code);
      const auto& cur_method_name = show(m);
      for (auto* block : cfg->blocks()) {
        auto vec = gather_source_blocks(block);
        for (auto* sb : vec) {
          auto it = std::find(print.begin(), print.end(), sb->src->str());
          if (it != std::end(print) && *it != cur_method_name) {
            TRACE(MMINL, 0, "### METHOD %s HAS SOURCE BLOCKS FROM %s ###",
                  cur_method_name.c_str(), (*it).c_str());
            log_cfg_violations(v, m->as_def(), ignore_undefined);
            return;
          }
        }
      }
    });
  }

  template <typename SpecialT>
  static void print_cfg_with_violations(DexMethod* m, bool ignore_undefined) {
    cfg::ScopedCFG cfg(m->get_code());
    SpecialT special{*cfg, ignore_undefined};
    TRACE(MMINL, 0, "=== %s ===\n%s\n", SHOW(m),
          show<SpecialT>(*cfg, special).c_str());
  }

  // This map stores SourceBlock -> Violation Type -> Vector of Strings of
  // Block Names where the violation is happening. Nullptr entry stores all
  // uncovered source blocks.
  using ViolationBlockMap =
      UnorderedMap<SourceBlock*,
                   UnorderedMap<Violation, std::vector<std::string>>>;

  template <typename SpecialT>
  static void gather_cfg_violating_blocks(DexMethod* m,
                                          ViolationBlockMap* violating_blocks,
                                          bool ignore_undefined) {
    cfg::ScopedCFG cfg(m->get_code());
    SpecialT special{*cfg, violating_blocks, ignore_undefined};

    const auto& blocks = cfg->blocks();
    // This needs a no-op stringstream to pass into the functions below
    std::ostringstream ss;
    for (auto* b : blocks) {
      special.start_block(ss, b);
      for (const auto& mie : *b) {
        special.mie_before(ss, mie);
        special.mie_after(ss, mie);
      }
      special.end_block(ss, b);
    }
  }

  static std::string_view get_violation_name(Violation v) {
    switch (v) {
    case Violation::kChainAndDom: {
      return "ChainAndDom";
    }
    case Violation::kHotImmediateDomNotHot: {
      return "HotImmediateDomNotHot";
    }
    case Violation::KHotAllChildrenCold: {
      return "HotAllChildrenCold";
    }
    case Violation::kHotMethodColdEntry: {
      return "HotMethodColdEntry";
    }
    case Violation::kUncoveredSourceBlocks: {
      return "UncoveredSourceBlocks";
    }
    case Violation::kHotNoHotPred: {
      return "HotNoHotPred";
    }
    case Violation::kUncoveredThrowDelineatedBlocks: {
      return "UncoveredThrowDelineatedBlocks";
    }
    case Violation::ViolationSize: {
      not_reached();
    }
    }
    not_reached();
  }

  static void print_cfg_with_all_violating_blocks(DexMethod* m,
                                                  bool ignore_undefined) {
    cfg::ScopedCFG cfg(m->get_code());
    ViolationBlockMap violation_blocks;
    for (Violation v = Violation::kHotImmediateDomNotHot;
         v < Violation::ViolationSize;
         v = static_cast<Violation>(static_cast<int>(v) + 1)) {
      // This is set so it does not actually print and clutter the output,
      // this is just used to call gather_cfg_violating_blocks

      log_cfg_violations(v, m, ignore_undefined, false, &violation_blocks);
    }

    std::stringstream ss;
    ss << "=== Violations ===\n";
    for (auto* block : cfg->blocks()) {
      auto source_block_vector = gather_source_blocks(block);
      bool block_has_violations = false;
      std::stringstream block_ss;
      block_ss << "B" << std::to_string(block->id()) << ":\n";
      for (auto* source_block : source_block_vector) {
        block_ss << " Source Block[" << source_block
                 << "]: " << source_block->src->c_str() << "@"
                 << source_block->id << ":\n";
        for (Violation v = Violation::kHotImmediateDomNotHot;
             v < Violation::ViolationSize;
             v = static_cast<Violation>(static_cast<int>(v) + 1)) {
          if (v != Violation::kUncoveredSourceBlocks &&
              v != Violation::kUncoveredThrowDelineatedBlocks) {
            block_ss << "  " << get_violation_name(v) << ": ";
            if (violation_blocks.count(source_block) != 0u) {
              block_has_violations = true;
              auto& violation_map = violation_blocks.at(source_block);
              if (violation_map.count(v) != 0u) {
                for (const auto& str : violation_map[v]) {
                  block_ss << str << ", ";
                }
              }
            }
            block_ss << '\n';
          }
        }
      }
      if (block_has_violations) {
        ss << block_ss.str() << '\n';
      }
    }
    // Add uncovered source blocks into the string
    if (violation_blocks.count(nullptr) != 0u) {
      auto& violation_map = violation_blocks.at(nullptr);
      Violation uncovered = Violation::kUncoveredSourceBlocks;
      if (violation_map.count(uncovered) != 0u) {
        ss << "  " << get_violation_name(uncovered) << ": ";
        for (const auto& str : violation_map[uncovered]) {
          ss << str << ", ";
        }
      }
      Violation uncovered_throw = Violation::kUncoveredThrowDelineatedBlocks;
      if (violation_map.count(uncovered_throw) != 0u) {
        ss << "  " << get_violation_name(uncovered_throw) << ": ";
        for (const auto& str : violation_map[uncovered_throw]) {
          ss << str << ", ";
        }
      }
    }
    TRACE(MMINL, 0, "%s\n", ss.str().c_str());
  }

  template <typename Derived>
  class ViolationVisitorBase {
   private:
    explicit ViolationVisitorBase(cfg::ControlFlowGraph& cfg,
                                  bool ignore_undefined)
        : violating_blocks(nullptr), ignore_undefined(ignore_undefined) {
      static_cast<Derived*>(this)->initialize(cfg);
    }

    explicit ViolationVisitorBase(cfg::ControlFlowGraph& cfg,
                                  ViolationBlockMap* violating_blocks,
                                  bool ignore_undefined)
        : violating_blocks(violating_blocks),
          ignore_undefined(ignore_undefined) {
      static_cast<Derived*>(this)->initialize(cfg);
    }

    friend Derived;

   protected:
    cfg::Block* cur{nullptr};
    ViolationBlockMap* violating_blocks{nullptr};
    bool ignore_undefined{false};

   public:
    void mie_before(std::ostream&, const MethodItemEntry&) {}

    void start_block(std::ostream& os, cfg::Block* b) {
      cur = b;
      static_cast<Derived*>(this)->on_start_block(os, b);
    }

    void end_block(std::ostream& os, cfg::Block* b) {
      static_cast<Derived*>(this)->on_end_block(os, b);
      cur = nullptr;
    }

    void mie_after(std::ostream& os, const MethodItemEntry& mie) {
      static_cast<Derived*>(this)->mie_after_impl(os, mie, ignore_undefined);
    }

   protected:
    void initialize(cfg::ControlFlowGraph&) {}
    void on_start_block(std::ostream&, cfg::Block*) {}
    void on_end_block(std::ostream&, cfg::Block*) {}
  };

  template <typename ViolationVisitor>
  static void handle_violation(DexMethod* m,
                               bool print_violations,
                               ViolationBlockMap* violation_blocks,
                               bool ignore_undefined) {
    if (print_violations) {
      print_cfg_with_violations<ViolationVisitor>(m, ignore_undefined);
    }
    if (violation_blocks != nullptr) {
      gather_cfg_violating_blocks<ViolationVisitor>(m, violation_blocks,
                                                    ignore_undefined);
    }
  }

  // This function can gather information on violating blocks (for all types
  // of violations) for each source block, as well as print out a
  // violation-labeled CFG (only for one violation at a time).
  static void log_cfg_violations(
      Violation v,
      DexMethod* m,
      bool ignore_undefined,
      bool print_violations = true,
      ViolationBlockMap* violation_blocks = nullptr) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot: {
      struct HotImmediateSpecial : ViolationVisitorBase<HotImmediateSpecial> {
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<HotImmediateSpecial>;

        explicit HotImmediateSpecial(cfg::ControlFlowGraph& cfg,
                                     bool ignore_undefined)
            : Base(cfg, ignore_undefined), dom(cfg) {}

        explicit HotImmediateSpecial(cfg::ControlFlowGraph& cfg,
                                     ViolationBlockMap* violating_blocks,
                                     bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined), dom(cfg) {}

        void mie_after_impl(std::ostream& os,
                            const MethodItemEntry& mie,
                            bool ignore_undefined) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          auto* immediate_dominator = dom.get_idom(cur);
          if (immediate_dominator == nullptr) {
            os << " NO DOMINATOR\n";
            return;
          }

          if (!source_blocks::has_source_block_positive_val(
                  mie.src_block.get())) {
            os << " NOT HOT\n";
            return;
          }

          auto* first_sb_immediate_dominator =
              source_blocks::get_first_source_block(immediate_dominator);
          if (first_sb_immediate_dominator == nullptr) {
            os << " NO DOMINATOR SOURCE BLOCK B" << immediate_dominator->id()
               << "\n";
            return;
          }

          bool is_idom_hot = source_blocks::has_source_block_positive_val(
              first_sb_immediate_dominator);
          if (is_idom_hot) {
            os << " DOMINATOR HOT\n";
            return;
          }

          if (ignore_undefined && source_blocks::has_source_block_undefined_val(
                                      first_sb_immediate_dominator)) {
            os << " IGNORING UNDEFINED\n";
            return;
          }

          if (violating_blocks != nullptr) {
            auto& map = (*violating_blocks)[mie.src_block.get()];
            map[Violation::kHotImmediateDomNotHot].emplace_back(
                "B" + std::to_string(immediate_dominator->id()));
          }
          os << " !!! B" << immediate_dominator->id() << ": ";
          auto* sb = first_sb_immediate_dominator;
          os << " \"" << show(sb->src) << "\"@" << sb->id;
          sb->foreach_val([&](const auto& val) {
            os << " ";
            if (val) {
              os << val->val << "/" << val->appear100;
            } else {
              os << "N/A";
            }
          });
          os << "\n";
        }
      };
      return handle_violation<HotImmediateSpecial>(
          m, print_violations, violation_blocks, ignore_undefined);
    }
    case Violation::kChainAndDom: {
      struct ChainAndDom : ViolationVisitorBase<ChainAndDom> {
        ChainAndDomState state{};
        bool first_in_block{false};
        bool prev_insn_can_throw{false};
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<ChainAndDom>;

        explicit ChainAndDom(cfg::ControlFlowGraph& cfg, bool ignore_undefined)
            : Base(cfg, ignore_undefined), dom(cfg) {}

        explicit ChainAndDom(cfg::ControlFlowGraph& cfg,
                             ViolationBlockMap* violating_blocks,
                             bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined), dom(cfg) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          Base::start_block(os, b);
          first_in_block = true;
        }

        void mie_after_impl(std::ostream& os,
                            const MethodItemEntry& mie,
                            bool ignore_undefined) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            prev_insn_can_throw =
                prev_insn_can_throw || (mie.type == MFLOW_OPCODE &&
                                        opcode::can_throw(mie.insn->opcode()));
            return;
          }

          size_t old_count = state.violations;

          auto* sb = mie.src_block.get();

          chain_and_dom_update<std::numeric_limits<uint32_t>::max()>(
              cur, sb, first_in_block, prev_insn_can_throw, state, dom,
              ignore_undefined);

          const bool head_error = state.violations > old_count;
          const auto* dom_block = state.dom_block;

          first_in_block = false;

          for (auto* cur_sb = sb->next.get(); cur_sb != nullptr;
               cur_sb = cur_sb->next.get()) {
            chain_and_dom_update<std::numeric_limits<uint32_t>::max()>(
                cur, cur_sb, first_in_block, prev_insn_can_throw, state, dom,
                ignore_undefined);
          }

          prev_insn_can_throw = false;

          if (state.violations > old_count) {
            os << " !!!";
            if (head_error) {
              os << " HEAD";
              if (dom_block != nullptr) {
                if (violating_blocks != nullptr) {
                  auto& map = (*violating_blocks)[sb];
                  map[Violation::kChainAndDom].emplace_back(
                      "B" + std::to_string(dom_block->id()));
                }
                os << " (B" << dom_block->id() << ")";
              }
            }
            auto other = state.violations - old_count - (head_error ? 1 : 0);
            if (other > 0) {
              os << " IN_CHAIN " << other;
            }
            os << "\n";
          }
        }
      };
      return handle_violation<ChainAndDom>(m, print_violations,
                                           violation_blocks, ignore_undefined);
    }
    case Violation::kUncoveredSourceBlocks: {
      struct UncoveredSourceBlocks
          : ViolationVisitorBase<UncoveredSourceBlocks> {
        using Base = ViolationVisitorBase<UncoveredSourceBlocks>;

        explicit UncoveredSourceBlocks(cfg::ControlFlowGraph& cfg,
                                       bool ignore_undefined)
            : Base(cfg, ignore_undefined) {}

        explicit UncoveredSourceBlocks(cfg::ControlFlowGraph& cfg,
                                       ViolationBlockMap* violating_blocks,
                                       bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined) {}

        void mie_after_impl(std::ostream&,
                            const MethodItemEntry&,
                            bool ignore_undefined) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          // Don't call Base::start_block - custom logic only
          if (is_ghost_block(b)) {
            return;
          }

          if (get_first_source_block(b) == nullptr) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[nullptr];
              map[Violation::kUncoveredSourceBlocks].emplace_back(
                  "B" + std::to_string(b->id()));
            }
            os << "!!!MISSING SOURCE BLOCK\n";
          }
        }

        void end_block(std::ostream& os, cfg::Block* b) {
          // Empty - don't call Base::end_block
        }
      };
      return handle_violation<UncoveredSourceBlocks>(
          m, print_violations, violation_blocks, ignore_undefined);
    }
    case Violation::kHotMethodColdEntry: {
      struct HotMethodColdEntry : ViolationVisitorBase<HotMethodColdEntry> {
        bool is_entry_block{false};
        bool first_in_block{false};

        using Base = ViolationVisitorBase<HotMethodColdEntry>;

        explicit HotMethodColdEntry(cfg::ControlFlowGraph& cfg,
                                    bool ignore_undefined)
            : Base(cfg, ignore_undefined) {}

        explicit HotMethodColdEntry(cfg::ControlFlowGraph& cfg,
                                    ViolationBlockMap* violating_blocks,
                                    bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          Base::start_block(os, b);
          is_entry_block = b->preds().empty();
          first_in_block = true;
        }

        void end_block(std::ostream& os, cfg::Block* b) {
          Base::end_block(os, b);
        }

        void mie_after_impl(std::ostream& os,
                            const MethodItemEntry& mie,
                            bool ignore_undefined) {
          if (mie.type != MFLOW_SOURCE_BLOCK || !is_entry_block ||
              !first_in_block) {
            return;
          }
          first_in_block = false;

          auto* sb = mie.src_block.get();
          bool violation_found_in_head{false};
          sb->foreach_val(
              [&violation_found_in_head, ignore_undefined](const auto& val) {
                if (!ignore_undefined && !val) {
                  violation_found_in_head = true;
                }
                if (!val || (val->appear100 != 0 && val->val == 0)) {
                  violation_found_in_head = true;
                }
              });
          if (violation_found_in_head) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[sb];
              map[Violation::kHotMethodColdEntry].emplace_back("HEAD");
            }
            os << " !!! HEAD SB: METHOD IS HOT BUT ENTRY IS COLD";
          }

          bool violation_found_in_chain{false};
          for (auto* cur_sb = sb->next.get(); cur_sb != nullptr;
               cur_sb = cur_sb->next.get()) {
            cur_sb->foreach_val([&violation_found_in_chain](const auto& val) {
              if (val && val->appear100 != 0 && val->val == 0) {
                violation_found_in_chain = true;
              }
            });
          }
          if (violation_found_in_chain) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[sb];
              map[Violation::kHotMethodColdEntry].emplace_back("CHAIN");
            }
            os << " !!! CHAIN SB: METHOD IS HOT BUT ENTRY IS COLD";
          }
          os << "\n";
        }
      };
      return handle_violation<HotMethodColdEntry>(
          m, print_violations, violation_blocks, ignore_undefined);
    }
    case Violation::kHotNoHotPred: {
      struct HotNoHotPred : ViolationVisitorBase<HotNoHotPred> {
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<HotNoHotPred>;

        explicit HotNoHotPred(cfg::ControlFlowGraph& cfg, bool ignore_undefined)
            : Base(cfg, ignore_undefined), dom(cfg) {}

        explicit HotNoHotPred(cfg::ControlFlowGraph& cfg,
                              ViolationBlockMap* violating_blocks,
                              bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined), dom(cfg) {}

        void mie_after_impl(std::ostream& os,
                            const MethodItemEntry& mie,
                            bool ignore_undefined) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          if (source_blocks::has_source_block_undefined_val(
                  mie.src_block.get()) &&
              ignore_undefined) {
            os << " IGNORE UNDEFINED\n";
            return;
          }

          if (!source_blocks::has_source_block_positive_val(
                  mie.src_block.get())) {
            os << " NOT HOT\n";
            return;
          }

          bool violation_found = true;

          if (cur->preds().empty()) {
            violation_found = false;
          }

          std::vector<std::string> cold_preds;
          for (auto* predecessor : cur->preds()) {
            auto* first_sb_pred =
                source_blocks::get_first_source_block(predecessor->src());
            if (source_blocks::has_source_block_positive_val(first_sb_pred) ||
                (ignore_undefined &&
                 source_blocks::has_source_block_undefined_val(
                     first_sb_pred))) {
              violation_found = false;
              break;
            } else {
              if (predecessor->src() != nullptr &&
                  violating_blocks != nullptr) {
                cold_preds.emplace_back(
                    "B" + std::to_string(predecessor->src()->id()));
              }
            }
          }

          if (violation_found) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[mie.src_block.get()];
              map[Violation::kHotNoHotPred] = cold_preds;
            }
            os << " !!! HOT BLOCK NO HOT PRED\n";
          }
        }
      };
      return handle_violation<HotNoHotPred>(m, print_violations,
                                            violation_blocks, ignore_undefined);
    }
    case Violation::KHotAllChildrenCold: {
      struct HotAllChildrenCold : ViolationVisitorBase<HotAllChildrenCold> {
        using Base = ViolationVisitorBase<HotAllChildrenCold>;

        explicit HotAllChildrenCold(cfg::ControlFlowGraph& cfg,
                                    bool ignore_undefined)
            : Base(cfg, ignore_undefined) {}

        explicit HotAllChildrenCold(cfg::ControlFlowGraph& cfg,
                                    ViolationBlockMap* violating_blocks,
                                    bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined) {}

        void mie_after_impl(std::ostream& os,
                            const MethodItemEntry& mie,
                            bool ignore_undefined) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          auto* last_sb_before_throw =
              source_blocks::get_last_source_block_if_after_throw(cur);

          if (mie.src_block.get() != last_sb_before_throw) {
            return;
          }

          if ((last_sb_before_throw == nullptr) ||
              !has_source_block_positive_val(last_sb_before_throw)) {
            return;
          }

          if (ignore_undefined &&
              has_source_block_undefined_val(last_sb_before_throw)) {
            return;
          }

          os << " HOT\n";

          bool has_successor = false;
          std::vector<std::string> cold_succs;
          for (auto* successor : cur->succs()) {
            auto* first_sb_succ =
                source_blocks::get_first_source_block(successor->src());
            has_successor = true;
            if (has_source_block_positive_val(first_sb_succ)) {
              return;
            }
            if (ignore_undefined &&
                has_source_block_undefined_val(first_sb_succ)) {
              return;
            }
            if (violating_blocks != nullptr) {
              cold_succs.emplace_back("B" +
                                      std::to_string(successor->src()->id()));
            }
          }
          if (has_successor) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[last_sb_before_throw];
              map[Violation::KHotAllChildrenCold] = cold_succs;
            }
            os << " !!! HOT ALL CHILDREN COLD\n";
          }
        }
      };
      return handle_violation<HotAllChildrenCold>(
          m, print_violations, violation_blocks, ignore_undefined);
    }
    case Violation::kUncoveredThrowDelineatedBlocks: {
      struct UncoveredThrowDelineated
          : ViolationVisitorBase<UncoveredThrowDelineated> {
        bool had_sb{false};

        using Base = ViolationVisitorBase<UncoveredThrowDelineated>;

        explicit UncoveredThrowDelineated(cfg::ControlFlowGraph& cfg,
                                          bool ignore_undefined)
            : Base(cfg, ignore_undefined) {}

        explicit UncoveredThrowDelineated(cfg::ControlFlowGraph& cfg,
                                          ViolationBlockMap* violating_blocks,
                                          bool ignore_undefined)
            : Base(cfg, violating_blocks, ignore_undefined) {}

        void on_start_block(std::ostream&, cfg::Block*) { had_sb = false; }

        void on_end_block(std::ostream& os, cfg::Block* b) {
          if (!had_sb && !b->empty()) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[nullptr];
              map[Violation::kUncoveredThrowDelineatedBlocks].emplace_back(
                  "B" + std::to_string(cur->id()));
            }
            os << "!!!UNCOVERED THROW DELINEATED BLOCK\n";
          }
        }

        void mie_after_impl(std::ostream&,
                            const MethodItemEntry&,
                            bool /*ignore_undefined*/) {}

        void mie_before(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type == MFLOW_SOURCE_BLOCK) {
            had_sb = true;
            return;
          }
          if (mie.type == MFLOW_OPCODE &&
              opcode::can_throw(mie.insn->opcode()) &&
              mie.insn->opcode() != OPCODE_THROW) {
            if (!had_sb) {
              if (violating_blocks != nullptr) {
                auto& map = (*violating_blocks)[nullptr];
                map[Violation::kUncoveredThrowDelineatedBlocks].emplace_back(
                    "B" + std::to_string(cur->id()));
              }
              os << "!!!UNCOVERED THROW DELINEATED BLOCK\n";
            }
            had_sb = false;
          }
        }
      };
      return handle_violation<UncoveredThrowDelineated>(
          m, print_violations, violation_blocks, ignore_undefined);
    }
    case Violation::ViolationSize:
      not_reached();
    }
    not_reached();
  }
};

ViolationsHelper::ViolationsHelper(Violation v,
                                   const Scope& scope,
                                   size_t top_n,
                                   std::vector<std::string> to_vis,
                                   bool track_intermethod_violations,
                                   bool print_all_violations,
                                   bool ignore_undefined)
    : impl(std::make_unique<ViolationsHelperImpl>(v,
                                                  scope,
                                                  top_n,
                                                  std::move(to_vis),
                                                  track_intermethod_violations,
                                                  print_all_violations,
                                                  ignore_undefined)) {}
ViolationsHelper::~ViolationsHelper() {}

void ViolationsHelper::process(ScopedMetrics* sm) {
  if (impl) {
    impl->process(sm);
  }
}
void ViolationsHelper::silence() {
  if (impl) {
    impl->silence();
  }
}

ViolationsHelper::ViolationsHelper(ViolationsHelper&& other) noexcept {
  impl = std::move(other.impl);
}
ViolationsHelper& ViolationsHelper::operator=(ViolationsHelper&& rhs) noexcept {
  impl = std::move(rhs.impl);
  return *this;
}

size_t compute(ViolationsHelper::Violation v,
               cfg::ControlFlowGraph& cfg,
               bool ignore_undefined) {
  return ViolationsHelper::ViolationsHelperImpl::compute(v, cfg,
                                                         ignore_undefined);
}

} // namespace source_blocks
