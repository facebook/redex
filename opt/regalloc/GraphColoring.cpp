/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "GraphColoring.h"

#include <boost/pending/disjoint_sets.hpp>
#include <boost/property_map/property_map.hpp>

#include "Debug.h"
#include "DexUtil.h"
#include "VirtualRegistersFile.h"

namespace regalloc {

/*
 * Given an invoke opcode, returns the number of virtual registers that it
 * requires for its sources.
 */
static size_t sum_src_sizes(const IRInstruction* insn) {
  size_t size{0};
  if (insn->opcode() != OPCODE_INVOKE_STATIC) {
    // Account for the implicit `this` parameter
    ++size;
  }
  auto& types = insn->get_method()->get_proto()->get_args()->get_type_list();
  for (auto* type : types) {
    size += is_wide_type(type) ? 2 : 1;
  }
  return size;
}

/*
 * Gathers all the instructions that must be encoded in range form.
 */
RangeSet init_range_set(IRCode* code) {
  RangeSet range_set;
  for (const auto& mie : InstructionIterable(code)) {
    const auto* insn = mie.insn;
    auto op = insn->opcode();
    bool is_range{false};
    if (op == OPCODE_FILLED_NEW_ARRAY) {
      is_range = insn->srcs_size() > opcode::NON_RANGE_MAX;
    } else if (is_invoke(op)) {
      is_range = sum_src_sizes(insn) > opcode::NON_RANGE_MAX;
    }
    if (is_range) {
      range_set.emplace(insn);
    }
  }
  return range_set;
}

namespace graph_coloring {

namespace {

/*
 * Given a node in the interference graph, mark all the vregs in the
 * register file that have been allocated to adjacent neighbors.
 */
void mark_adjacent(const interference::Graph& ig,
                   reg_t reg,
                   const transform::RegMap& reg_map,
                   VirtualRegistersFile* vreg_file) {
  for (auto adj : ig.get_node(reg).adjacent()) {
    auto it = reg_map.find(adj);
    if (it != reg_map.end()) {
      vreg_file->alloc_at(it->second, ig.get_node(adj).width());
    }
  }
}

constexpr int INVALID_SCORE = std::numeric_limits<int>::max();

/*
 * Count the number of vregs we would need to spill if we allocated a
 * contiguous range of vregs starting at :range_base.
 */
int score_range_fit(
    const interference::Graph& ig,
    const std::vector<reg_t>& range_regs,
    reg_t range_base,
    const std::unordered_map<reg_t, VirtualRegistersFile>& vreg_files,
    const transform::RegMap& reg_map) {
  int score{0};
  auto vreg = range_base;
  for (size_t i = 0; i < range_regs.size(); ++i) {
    auto reg = range_regs.at(i);
    const auto& node = ig.get_node(reg);
    const auto& vreg_file = vreg_files.at(reg);
    auto it = reg_map.find(reg);
    // XXX We could be more precise here by checking the LivenessDomain for the
    // given range instruction instead of just using the graph
    if (!vreg_file.is_free(vreg, node.width())) {
      return INVALID_SCORE;
    }
    if ((it != reg_map.end() && it->second != vreg) || vreg > node.max_vreg()) {
      ++score;
    }
    vreg += node.width();
  }
  return score;
}

/*
 * Searches between :range_base_start and :range_base_end, and returns the
 * range_base with the best score.
 */
reg_t find_best_range_fit(
    const interference::Graph& ig,
    const std::vector<reg_t>& range_regs,
    reg_t range_base_start,
    reg_t range_base_end,
    const std::unordered_map<reg_t, VirtualRegistersFile>& vreg_files,
    const transform::RegMap& reg_map) {
  int min_score{INVALID_SCORE};
  reg_t range_base = 0;
  for (reg_t i = range_base_start; i <= range_base_end; ++i) {
    auto score = score_range_fit(ig, range_regs, i, vreg_files, reg_map);
    if (score < min_score) {
      min_score = score;
      range_base = i;
    }
    if (min_score == 0) {
      break;
    }
  }
  always_assert(min_score != INVALID_SCORE);
  return range_base;
}

/*
 * Map a range instruction such that it starts at :range_base. Insert spills
 * as necessary.
 */
void fit_range_instruction(
    const interference::Graph& ig,
    const IRInstruction* insn,
    reg_t range_base,
    const std::unordered_map<reg_t, VirtualRegistersFile>& vreg_files,
    RegisterTransform* reg_transform,
    SpillPlan* spills) {
  auto vreg = range_base;
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto src = insn->src(i);
    const auto& node = ig.get_node(src);
    const auto& vreg_file = vreg_files.at(src);
    auto& reg_map = reg_transform->map;
    // If the vreg we're trying to map the node to is too large, or if the node
    // has been mapped to a different vreg already, we need to spill it.
    if (vreg > node.max_vreg() || reg_map.find(src) != reg_map.end()) {
      spills->range_spills[insn].emplace(src);
    } else {
      always_assert(vreg_file.is_free(vreg, node.width()));
      reg_map.emplace(src, vreg);
    }
    vreg += node.width();
  }
  reg_transform->size = std::max(reg_transform->size, vreg);
}

/*
 * Map the parameters such that they start at :param_base. Insert spills as
 * necessary.
 */
void fit_params(
    const interference::Graph& ig,
    const boost::sub_range<FatMethod>& param_insns,
    reg_t params_base,
    const std::unordered_map<reg_t, VirtualRegistersFile>& vreg_files,
    RegisterTransform* reg_transform,
    SpillPlan* spills) {
  auto vreg = params_base;
  for (const auto& mie : InstructionIterable(param_insns)) {
    auto* insn = mie.insn;
    auto dest = insn->dest();
    const auto& node = ig.get_node(dest);
    const auto& vreg_file = vreg_files.at(dest);
    auto& reg_map = reg_transform->map;
    // If the vreg we're trying to map the node to is too large, or if the node
    // has been mapped to a different vreg already, we need to spill it.
    if (vreg > node.max_vreg() || reg_map.find(dest) != reg_map.end()) {
      spills->param_spills.emplace(dest);
    } else {
      always_assert(vreg_file.is_free(vreg, node.width()));
      reg_map.emplace(dest, vreg);
    }
    vreg += node.width();
  }
  reg_transform->size = std::max(reg_transform->size, vreg);
}

std::string show(const SpillPlan& spill_plan) {
  std::ostringstream ss;
  ss << "Global spills:\n";
  for (auto pair : spill_plan.global_spills) {
    ss << pair.first << " -> " << pair.second << "\n";
  }
  ss << "Param spills:\n";
  for (auto reg : spill_plan.param_spills) {
    ss << reg << "\n";
  }
  ss << "Range spills:\n";
  for (auto pair : spill_plan.range_spills) {
    ss << show(pair.first) << ": ";
    for (auto reg : pair.second) {
      ss << reg << " ";
    }
    ss << "\n";
  }
  return ss.str();
}

std::string show(const interference::Graph& ig) {
  std::ostringstream ss;
  ig.write_dot_format(ss);
  return ss.str();
}

std::string show(const RegisterTransform& reg_transform) {
  std::ostringstream ss;
  ss << "size: " << reg_transform.size << "\n";
  for (auto pair : reg_transform.map) {
    ss << pair.first << " -> " << pair.second << "\n";
  }
  return ss.str();
}

} // namespace

void Allocator::Stats::accumulate(const Allocator::Stats& that) {
  reiteration_count += that.reiteration_count;
  param_spill_moves += that.param_spill_moves;
  range_spill_moves += that.range_spill_moves;
  global_spill_moves += that.global_spill_moves;
  moves_coalesced += that.moves_coalesced;
}

static bool has_2addr_form(DexOpcode op) {
  return op >= OPCODE_ADD_INT && op <= OPCODE_REM_DOUBLE;
}

/*
 * Coalesce symregs when there is potential for a more compact encoding. There
 * are 3 kinds of instructions that have this opportunity:
 *
 *   * move instructions whose src and dest don't interfere can be removed
 *
 *   * instructions like add-int whose src(0) and dest don't interfere may
 *     be encoded as add-int/2addr
 *
 *   * check-cast instructions with identical src and dest won't need to be
 *     preceded by a move opcode in the output
 *
 * Coalescing means that we combine the interference graph nodes. If we have a
 * move instruction, we remove it here. We shouldn't convert potentially
 * 2addr-eligible opcodes to that form here because they ultimately may need
 * the larger non-2addr encoding if their assigned vregs are larger than 4
 * bits. They will be handled in the post-regalloc instruction selection phase.
 *
 * Return a bool indicating whether any coalescing was done.
 *
 * This is fairly similar to the implementation in [Briggs92] section 8.6.
 */
bool Allocator::coalesce(interference::Graph* ig, IRCode* code) {
  // XXX We could use something more compact than an unordered_map?
  using Rank = std::unordered_map<reg_t, size_t>;
  using Parent = std::unordered_map<reg_t, reg_t>;
  using RankPMap = boost::associative_property_map<Rank>;
  using ParentPMap = boost::associative_property_map<Parent>;
  using RegisterAliasSets = boost::disjoint_sets<RankPMap, ParentPMap>;

  // Every time we coalesce a pair of symregs, we put them into the same
  // union-find tree. At the end of the coalescing process, we will map all the
  // symregs in each set to the root of that tree.
  Rank rank;
  Parent parent;
  RegisterAliasSets aliases((RankPMap(rank)), (ParentPMap(parent)));
  for (size_t i = 0; i < code->get_registers_size(); ++i) {
    aliases.make_set(i);
  }

  auto ii = InstructionIterable(code);
  auto end = ii.end();
  auto old_coalesce_count = m_stats.moves_coalesced;
  for (auto it = ii.begin(); it != end; ++it) {
    auto insn = it->insn;
    auto op = insn->opcode();
    if (!is_move(op) && !has_2addr_form(op) && op != OPCODE_CHECK_CAST) {
      continue;
    }
    auto dest = aliases.find_set(insn->dest());
    auto src = aliases.find_set(insn->src(0));
    if (dest == src) {
      if (is_move(op)) {
        ++m_stats.moves_coalesced;
        code->remove_opcode(it.unwrap());
      }
    } else if (ig->is_coalesceable(dest, src)) {
      // This unifies the two trees represented by dest and src
      aliases.link(dest, src);
      // Since link() doesn't tell us whether dest or src is the root of the
      // newly merged trees, we have to use find_set() to figure that out.
      auto parent = dest;
      auto child = src;
      if (aliases.find_set(dest) != dest) {
        std::swap(parent, child);
      }
      // Merge the child's node into the parent's
      ig->combine(parent, child);
      if (is_move(op)) {
        ++m_stats.moves_coalesced;
        code->remove_opcode(it.unwrap());
      }
    }
  }

  transform::RegMap reg_map;
  for (size_t i = 0; i < code->get_registers_size(); ++i) {
    reg_map.emplace(i, aliases.find_set(i));
  }
  transform::remap_registers(code, reg_map);

  return m_stats.moves_coalesced != old_coalesce_count;
}

/*
 * Simplify the graph: remove nodes of low weight repeatedly until none are
 * left, then remove nodes of high weight (which will hopefully create more
 * nodes of low weight).
 *
 * Nodes that are used by load-param or range opcodes are ignored.
 *
 * This is fairly similar to section 8.8 in [Briggs92], except we are using
 * a weight as given by [Smith00] instead of just the node's degree.
 */
void Allocator::simplify(interference::Graph* ig,
                         std::stack<reg_t>* select_stack) {
  // Nodes of low weight that we know are colorable. Note that even if all
  // the nodes in `low` have a max_vreg of 15, we can still have more than 16
  // of them here since some of them can have zero weight.
  std::set<reg_t> low;
  // Nodes that may not be colorable
  std::set<reg_t> high;

  // XXX: We may benefit from having a custom sorting function for the sets
  // above

  for (const auto& pair : ig->active_nodes()) {
    auto reg = pair.first;
    auto& node = pair.second;
    if (node.is_param() || node.is_range()) {
      continue;
    }
    if (node.definitely_colorable()) {
      low.emplace(reg);
    } else {
      high.emplace(reg);
    }
  }
  while (true) {
    while (low.size() > 0) {
      auto reg = *low.begin();
      const auto& node = ig->get_node(reg);
      TRACE(REG, 6, "Removing %u\n", reg);
      select_stack->push(reg);
      ig->remove_node(reg);
      low.erase(reg);
      for (auto adj : node.adjacent()) {
        auto& adj_node = ig->get_node(adj);
        if (!adj_node.is_active() || adj_node.is_param() ||
            adj_node.is_range()) {
          continue;
        }
        if (adj_node.definitely_colorable()) {
          low.emplace(adj);
          high.erase(adj);
        }
      }
    }
    if (high.size() == 0) {
      break;
    }
    auto spill_candidate_it =
        std::find_if(high.begin(), high.end(), [ig](reg_t reg) {
          return !ig->get_node(reg).is_spilt();
        });
    if (spill_candidate_it == high.end()) {
      spill_candidate_it = high.begin();
    }
    TRACE(REG, 6, "Potentially spilling %u\n", *high.begin());
    // Our spill candidate has too many neighbors for us to be certain that we
    // can color it. Instead of spilling it immediately, we put it into `low`,
    // which will ensure that it ends up on the stack before any of the
    // neighbors that cause it to have a high weight. Then when we're running
    // select(), by the time we re-encounter this node, we've colored all those
    // neighbors. If some of those neighbors share the same colors, we may be
    // able to color this node despite its weight. Briggs calls this
    // "optimistic coloring".
    low.emplace(*spill_candidate_it);
    high.erase(*spill_candidate_it);
  }
}

/*
 * Assign virtual registers to our symbolic registers, spilling where
 * necessary.
 *
 * Range- and param-related symregs are not handled here.
 */
void Allocator::select(const IRCode* code,
                       const interference::Graph& ig,
                       std::stack<reg_t>* select_stack,
                       RegisterTransform* reg_transform,
                       SpillPlan* spill_plan) {
  reg_t vregs_size{0};
  while (!select_stack->empty()) {
    auto reg = select_stack->top();
    select_stack->pop();
    auto& node = ig.get_node(reg);
    VirtualRegistersFile vreg_file;
    mark_adjacent(ig, reg, reg_transform->map, &vreg_file);
    auto vreg = vreg_file.alloc(node.width());
    if (vreg <= node.max_vreg()) {
      reg_transform->map.emplace(reg, vreg);
    } else {
      spill_plan->global_spills.emplace(reg, vreg);
    }
    vregs_size = std::max(vregs_size, vreg_file.size());
  }
  reg_transform->size = vregs_size;
}

/*
 * Ad-hoc heuristic: if we are going to be able to allocate a non-range
 * instruction with N operands without spilling, we must have N vregs that are
 * not live-out at that instruction. So range-ify the instruction if that is
 * not true. This is a liberal heuristic, since the N operands may interfere at
 * other instructions and fail to find a slot that's < 16.
 *
 * Wide operands further complicate things, since they may not fit even when
 * there are N available vregs. Right now we just range-ify any instruction
 * that references a wide reg.
 */
bool should_convert_to_range(const interference::Graph& ig,
                             const SpillPlan& spill_plan,
                             const IRInstruction* insn) {
  if (!opcode::has_range_form(insn->opcode())) {
    return false;
  }
  constexpr reg_t NON_RANGE_MAX_VREG = 15;
  bool has_wide{false};
  bool has_spill{false};
  std::unordered_set<reg_t> src_reg_set;
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto src = insn->src(i);
    src_reg_set.emplace(src);
    auto& node = ig.get_node(src);
    if (node.width() > 1) {
      has_wide = true;
    }
    if (spill_plan.global_spills.count(src)) {
      has_spill = true;
    }
  }
  if (!has_spill) {
    return false;
  }
  if (has_wide) {
    return true;
  }

  auto& liveness = ig.get_liveness(insn);
  reg_t low_regs_occupied{0};
  for (auto reg : liveness.elements()) {
    auto& node = ig.get_node(reg);
    if (node.max_vreg() > NON_RANGE_MAX_VREG || src_reg_set.count(reg)) {
      continue;
    }
    if (node.width() > 1) {
      return true;
    }
    ++low_regs_occupied;
  }
  return insn->srcs_size() + low_regs_occupied > NON_RANGE_MAX_VREG + 1;
}

void Allocator::choose_range_promotions(const IRCode* code,
                                        const interference::Graph& ig,
                                        const SpillPlan& spill_plan,
                                        RangeSet* range_set) {
  for (const auto& mie : InstructionIterable(code)) {
    const auto* insn = mie.insn;
    if (should_convert_to_range(ig, spill_plan, insn)) {
      range_set->emplace(insn);
    }
  }
}

/*
 * Assign virtual registers to our symbolic range-related registers, spilling
 * where necessary. We try to align the various ranges to minimize spillage.
 *
 * Since range instructions can address operands of any size, we run this after
 * allocating non-range-related nodes, so that the non-range ones have priority
 * in consuming the low vregs.
 */
void Allocator::select_ranges(const IRCode* code,
                              const interference::Graph& ig,
                              const RangeSet& range_set,
                              RegisterTransform* reg_transform,
                              SpillPlan* spill_plan) {
  for (auto* insn : range_set) {
    TRACE(REG, 5, "Allocating %s as range kind\n", SHOW(insn));
    std::unordered_map<reg_t, VirtualRegistersFile> vreg_files;
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      VirtualRegistersFile vreg_file;
      auto src = insn->src(i);
      mark_adjacent(ig, src, reg_transform->map, &vreg_file);
      vreg_files.emplace(src, vreg_file);
    }

    reg_t range_base = find_best_range_fit(ig,
                                           insn->srcs(),
                                           0,
                                           reg_transform->size,
                                           vreg_files,
                                           reg_transform->map);
    fit_range_instruction(
        ig, insn, range_base, vreg_files, reg_transform, spill_plan);
  }
}

/*
 * Assign virtual registers to our symbolic param-related registers, spilling
 * where necessary.
 */
void Allocator::select_params(const IRCode* code,
                              const interference::Graph& ig,
                              RegisterTransform* reg_transform,
                              SpillPlan* spill_plan) {
  std::unordered_map<reg_t, VirtualRegistersFile> vreg_files;
  std::vector<reg_t> param_regs;
  auto param_insns = code->get_param_instructions();
  size_t params_size{0};
  for (auto& mie : InstructionIterable(param_insns)) {
    auto dest = mie.insn->dest();
    const auto& node = ig.get_node(dest);
    params_size += node.width();
    param_regs.emplace_back(dest);
    VirtualRegistersFile vreg_file;
    mark_adjacent(ig, dest, reg_transform->map, &vreg_file);
    vreg_files.emplace(dest, vreg_file);
  }

  auto min_param_reg =
      reg_transform->size < params_size ? 0 : reg_transform->size - params_size;
  auto params_base = find_best_range_fit(ig,
                                         param_regs,
                                         min_param_reg,
                                         reg_transform->size,
                                         vreg_files,
                                         reg_transform->map);
  fit_params(
      ig, param_insns, params_base, vreg_files, reg_transform, spill_plan);
}

/*
 * Insert loads before every use of a globally spilled symreg, and stores
 * after a def.
 *
 * In order to minimize the number of spills, range-related symregs are spilled
 * by inserting loads just before the range instruction. Other instructions
 * that use those symregs will not be affected. This changes one range-related
 * symreg into one range-related and one normal one; if the normal symreg still
 * can't be allocated, it will get globally spilled on the next iteration of
 * the allocation loop.
 *
 * Param-related symregs are spilled by inserting loads just after the
 * block of parameter instructions. XXX We should probably optimize this by
 * inserting loads on the first use of those symregs...
 */
void Allocator::spill(const interference::Graph& ig,
                      const SpillPlan& spill_plan,
                      const RangeSet& range_set,
                      IRCode* code) {
  // TODO: account for "close" defs and uses. See [Briggs92], section 8.7

  std::unordered_set<reg_t> new_temps;
  // Spill param symregs
  auto param_insns = InstructionIterable(code->get_param_instructions());
  auto pend = param_insns.end();
  std::vector<IRInstruction*> param_moves;
  for (auto it = param_insns.begin(); it != pend; ++it) {
    auto insn = it->insn;
    auto dest = insn->dest();
    if (spill_plan.param_spills.find(dest) != spill_plan.param_spills.end()) {
      auto temp = code->allocate_temp();
      insn->set_dest(temp);
      new_temps.emplace(temp);
      param_moves.emplace_back(gen_move(ig.get_node(dest).type(), dest, temp));
      ++m_stats.param_spill_moves;
    }
  }
  for (auto* insn : param_moves) {
    code->insert_before(pend.unwrap(), insn);
  }

  auto ii = InstructionIterable(code);
  auto end = ii.end();
  for (auto it = ii.begin(); it != end; ++it) {
    auto* insn = it->insn;
    if (range_set.contains(insn)) {
      // Spill range symregs
      auto to_spill_it = spill_plan.range_spills.find(insn);
      if (to_spill_it != spill_plan.range_spills.end()) {
        auto& to_spill = to_spill_it->second;
        for (size_t i = 0; i < insn->srcs_size(); ++i) {
          auto src = insn->src(i);
          if (to_spill.find(src) == to_spill.end()) {
            continue;
          }
          auto& node = ig.get_node(src);
          auto temp = code->allocate_temp();
          insn->set_src(i, temp);
          new_temps.emplace(temp);
          auto mov = gen_move(node.type(), temp, src);
          ++m_stats.range_spill_moves;
          code->insert_before(it.unwrap(), mov);
        }
      }
    } else {
      // Spill non-param, non-range symregs
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src = insn->src(i);
        // We've already spilt this when handling range / param nodes above
        if (new_temps.find(src) != new_temps.end()) {
          continue;
        }
        auto& node = ig.get_node(src);
        auto sp_it = spill_plan.global_spills.find(src);
        auto max_value = max_unsigned_value(insn->src_bit_width(i));
        if (is_invoke(insn->opcode()) && node.width() == 2) {
          // We need to reserve one vreg for denormalization. See the
          // comments in GraphBuilder::update_node_constraints() for details.
          --max_value;
        }
        if (sp_it != spill_plan.global_spills.end() &&
            sp_it->second > max_value) {
          auto temp = code->allocate_temp();
          insn->set_src(i, temp);
          auto mov = gen_move(node.type(), temp, src);
          ++m_stats.global_spill_moves;
          code->insert_before(it.unwrap(), mov);
        }
      }
      if (insn->dests_size()) {
        auto dest = insn->dest();
        auto sp_it = spill_plan.global_spills.find(dest);
        if (sp_it != spill_plan.global_spills.end() &&
            sp_it->second > max_unsigned_value(insn->dest_bit_width())) {
          auto temp = code->allocate_temp();
          insn->set_dest(temp);
          it.reset(code->insert_after(
              it.unwrap(), gen_move(ig.get_node(dest).type(), dest, temp)));
          ++m_stats.global_spill_moves;
        }
      }
    }
  }
}

/*
 * Main differences from the standard Chaitin-Briggs
 * build-coalesce-simplify-spill loop:
 *
 *   * We only coalesce the first time around, because our move instructions
 *     and our spill / reload instructions are one and the same. This is
 *     easily fixable, though I have yet to profile the performance tradeoff.
 *     We also don't rebuild the interference graph after coalescing; I'd
 *     like to do some performance work before enabling that.
 *
 *   * We have to handle range instructions and have the parameter vregs
 *     at the end of the frame, which the original algorithm doesn't quite
 *     account for. These are handled in select_ranges and select_params
 *     respectively.
 */
void Allocator::allocate(IRCode* code) {

  // Any temp larger than this is the result of the spilling process
  auto initial_regs = code->get_registers_size();

  // The set of instructions that will be encoded in range form. This is a
  // monotonically increasing set, i.e. we only add and never remove from it
  // in the allocation loop below.
  auto range_set = init_range_set(code);

  bool first{true};
  while (true) {
    SpillPlan spill_plan;
    RegisterTransform reg_transform;

    TRACE(REG, 5, "Allocating:\n%s\n", SHOW(code->cfg()));
    auto ig = interference::build_graph(code, initial_regs, range_set);
    if (first) {
      coalesce(&ig, code);
      first = false;
    } else {
      // TODO we should coalesce here too, but we'll need to avoid removing
      // moves that were inserted by spilling

      // If we've hit this many iterations, it's very likely that we've hit
      // some bug that's causing us to loop infinitely.
      always_assert(m_stats.reiteration_count++ < 200);
    }
    TRACE(REG, 5, "Post-coalesce:\n%s\n", SHOW(code->cfg()));
    TRACE(REG, 7, "IG:\n%s", SHOW(ig));

    std::stack<reg_t> select_stack;
    simplify(&ig, &select_stack);
    select(code, ig, &select_stack, &reg_transform, &spill_plan);

    TRACE(REG, 5, "Transform before range alloc:\n%s\n", SHOW(reg_transform));
    choose_range_promotions(code, ig, spill_plan, &range_set);
    select_ranges(code, ig, range_set, &reg_transform, &spill_plan);
    select_params(code, ig, &reg_transform, &spill_plan);
    TRACE(REG, 5, "Transform after range alloc:\n%s\n", SHOW(reg_transform));

    if (!spill_plan.empty()) {
      TRACE(REG, 5, "Spill plan:\n%s\n", SHOW(spill_plan));
      spill(ig, spill_plan, range_set, code);
    } else {
      transform::remap_registers(code, reg_transform.map);
      code->set_registers_size(reg_transform.size);
      break;
    }
  }

  TRACE(REG, 3, "Reiteration count: %lu\n", m_stats.reiteration_count);
  TRACE(REG, 3, "Spill count: %lu\n", m_stats.moves_inserted());
  TRACE(REG, 3, "  Param spills: %lu\n", m_stats.param_spill_moves);
  TRACE(REG, 3, "  Range spills: %lu\n", m_stats.range_spill_moves);
  TRACE(REG, 3, "  Global spills: %lu\n", m_stats.global_spill_moves);
  TRACE(REG, 3, "Coalesce count: %lu\n", m_stats.moves_coalesced);
  TRACE(REG, 3, "Net moves: %ld\n", m_stats.net_moves());
}

} // namespace graph_coloring

} // namespace regalloc
