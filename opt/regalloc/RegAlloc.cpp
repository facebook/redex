/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RegAlloc.h"

#include <boost/functional/hash.hpp>

#include "Dataflow.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "Transform.h"
#include "Walkers.h"

IRInstruction* gen_move(RegisterKind kind, reg_t dest, reg_t src) {
  static std::unordered_map<RegisterKind,
                            std::array<DexOpcode, 3>,
                            boost::hash<RegisterKind>>
      move_map{
          {RegisterKind::NORMAL,
           {{OPCODE_MOVE, OPCODE_MOVE_FROM16, OPCODE_MOVE_16}}},
          {RegisterKind::WIDE,
           {{OPCODE_MOVE_WIDE, OPCODE_MOVE_WIDE_FROM16, OPCODE_MOVE_WIDE_16}}},
          {RegisterKind::OBJECT,
           {{OPCODE_MOVE_OBJECT,
             OPCODE_MOVE_OBJECT_FROM16,
             OPCODE_MOVE_OBJECT_16}}}};
  assert_log(move_map.find(kind) != move_map.end(),
      "Cannot generate move for register kind %s", SHOW(kind));
  DexOpcode op;
  if (required_bit_width(dest) <= 4 && required_bit_width(src) <= 4) {
    op = move_map.at(kind).at(0);
  } else if (required_bit_width(dest) <= 8) {
    op = move_map.at(kind).at(1);
  } else {
    op = move_map.at(kind).at(2);
  }
  auto insn = new IRInstruction(op);
  insn->set_dest(dest);
  insn->set_src(0, src);
  return insn;
}

void HighRegMoveInserter::Stats::add_move(IRInstruction* insn) {
  ++moves_inserted;
  bytes_added += insn->size();
}

HighRegMoveInserter::SwapInfo HighRegMoveInserter::reserve_swap(
    DexMethod* method) {
  SwapInfo info;
  auto code = method->get_code();
  auto low_regs_shortfall = low_reg_space_needed(&*code);
  auto range_shortfall = range_space_needed(&*code);
  while (low_regs_shortfall > 0 || range_shortfall > 0) {
    // XXX(jezng): increment_all_regs takes the number of regs to increment by,
    // while enlarge_regs takes the total number of regs after the
    // transformation... should make them uniform
    increment_all_regs(&*code, low_regs_shortfall);
    IRCode::enlarge_regs(method, code->get_registers_size() + range_shortfall);

    info.low_reg_swap += low_regs_shortfall;
    info.range_swap += range_shortfall;
    low_regs_shortfall = low_reg_space_needed(&*code) - info.low_reg_swap;
    range_shortfall = range_space_needed(&*code) - info.range_swap;
  }
  return info;
}

size_t HighRegMoveInserter::low_reg_space_needed(
    IRCode* code) {
  size_t rv = 0;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    auto op = insn->opcode();
    if (opcode::has_range_form(op)) {
      continue;
    }
    if (insn->dests_size() &&
        required_bit_width(insn->dest()) > insn->dest_bit_width()) {
      rv = std::max(rv, static_cast<size_t>(insn->dest_is_wide() ? 2  : 1));
    }
    size_t srcs_swap_needed = 0;
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (required_bit_width(insn->src(i)) > insn->src_bit_width(i)) {
        srcs_swap_needed += insn->src_is_wide(i) ? 2 : 1;
      }
    }
    rv = std::max(rv, srcs_swap_needed);
  }
  return rv;
}

size_t HighRegMoveInserter::range_space_needed(IRCode* code) {
  size_t rv = 0;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    auto op = insn->opcode();
    if (!opcode::has_range_form(op)) {
      continue;
    }
    size_t needed = 0;
    if (needs_range_conversion(insn) && !has_contiguous_srcs(insn)) {
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        needed += insn->src_is_wide(i) ? 2 : 1;
      }
    }
    rv = std::max(rv, needed);
  }
  return rv;
}

void HighRegMoveInserter::increment_all_regs(IRCode* code,
                                             size_t size) {
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      insn->set_dest(insn->dest() + size);
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, insn->src(i) + size);
    }
    if (opcode::has_range(insn->opcode())) {
      insn->set_range_base(insn->range_base() + size);
    }
  }
  code->set_registers_size(code->get_registers_size() + size);
}

/*
 * Ensure that all invoke/fill-array instructions have either =< 5 reg args, or
 * have > 5 args that are consecutive registers. The latter case will have
 * their opcodes converted to the /range version later.
 */
void HighRegMoveInserter::handle_rangeable(IRCode* code,
                                           InstructionIterator& it,
                                           const KindVec& reg_kinds,
                                           reg_t range_start) {
  auto insn = it->insn;
  if (!needs_range_conversion(insn)) {
    return;
  }

  TRACE(REG, 5, "%s needs to be converted to /range\n", SHOW(insn));
  if (has_contiguous_srcs(insn)) {
    return;
  }
  ++m_stats.range_conversions;
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto reg_kind = reg_kinds.at(insn->src(i));
    auto mov = gen_move(reg_kind, range_start + i, insn->src(i));
    code->insert_before(it.unwrap(), mov);
    m_stats.add_move(mov);
    auto old_src = insn->src(i);
    insn->set_src(i, range_start + i);

    // methods taking wide arguments have each register in the pair specified
    // as consecutive operands
    if (reg_kind == RegisterKind::WIDE) {
      always_assert(insn->src(++i) == old_src + 1);
      insn->set_src(i, range_start + i);
    }
  }
}

static std::string show_register_kinds(
    IRCode* code,
    const std::unordered_map<IRInstruction*, KindVec>& reg_kind_map) {
  std::stringstream ss;
  auto ii = InstructionIterable(code);
  auto end = ii.end();
  for (auto it = ii.begin(); it != end; ++it) {
    auto insn = it ->insn;
    ss << show(insn) << " ";
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      ss << show(reg_kind_map.at(insn).at(insn->src(i))) << " ";
    }
    ss << "\n";
  }
  return ss.str();
}

void HighRegMoveInserter::insert_moves(
    IRCode* code, const HighRegMoveInserter::SwapInfo& swap_info) {
  auto reg_kind_map = analyze_register_kinds(code);
  TRACE(REG, 5, "%s", show_register_kinds(&*code, *reg_kind_map).c_str());
  auto ii = InstructionIterable(code);
  auto end = ii.end();
  for (auto it = ii.begin(); it != end; ++it) {
    auto insn = it->insn;
    auto op = insn->opcode();
    TRACE(REG, 6, "Processing %s\n", SHOW(insn));
    if (opcode::has_range_form(op)) {
      auto reg_kinds = reg_kind_map->at(insn);
      auto range_start = code->get_registers_size() - sum_param_sizes(code) -
                         swap_info.range_swap;
      handle_rangeable(&*code, it, reg_kinds, range_start);
      continue;
    }
    size_t swap_used {0};
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (required_bit_width(insn->src(i)) > insn->src_bit_width(i)) {
        auto reg_kind = reg_kind_map->at(insn).at(insn->src(i));
        auto mov = gen_move(reg_kind, swap_used, insn->src(i));
        code->insert_before(it.unwrap(), mov);
        insn->set_src(i, swap_used);
        swap_used += reg_kind == RegisterKind::WIDE ? 2 : 1;
        m_stats.add_move(mov);
      }
    }
    if (insn->dests_size() &&
        required_bit_width(insn->dest()) > insn->dest_bit_width()) {
      auto reg_kind = dest_kind(insn->opcode());
      auto mov = gen_move(reg_kind, insn->dest(), 0);
      it.reset(code->insert_after(it.unwrap(), mov));
      insn->set_dest(0);
      m_stats.add_move(mov);
    }
  }
}

void RegAllocPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  HighRegMoveInserter move_inserter;
  walk_code(scope,
            [](DexMethod*) { return true; },
            [&](DexMethod* m, IRCode& code) {
              TRACE(REG, 3, "Allocating %s regs: %d\n",
                    SHOW(m), code.get_registers_size());
              try {
                for (auto& mie : InstructionIterable(&code)) {
                  mie.insn->range_to_srcs();
                }

                TRACE(REG, 5, "Before reservation:\n%s\n", SHOW(&code));
                auto swap_info = HighRegMoveInserter::reserve_swap(m);
                TRACE(REG, 3, "Swap info: %d %d\n",
                      swap_info.low_reg_swap,
                      swap_info.range_swap);
                TRACE(REG, 5, "After reservation:\n%s\n", SHOW(&code));
                move_inserter.insert_moves(&code, swap_info);

                for (auto& mie : InstructionIterable(&code)) {
                  mie.insn->srcs_to_range();
                }
              } catch (std::exception&) {
                fprintf(stderr, "Failed to allocate %s\n", SHOW(m));
                throw;
              }
            });
  auto& stats = move_inserter.get_stats();
  mgr.incr_metric("moves_inserted", stats.moves_inserted);
  mgr.incr_metric("range_conversions", stats.range_conversions);
  mgr.incr_metric("bytes_added", stats.bytes_added);
}

static RegAllocPass s_pass;
