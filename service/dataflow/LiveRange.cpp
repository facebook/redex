/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LiveRange.h"

#include <boost/pending/disjoint_sets.hpp>
#include <boost/property_map/property_map.hpp>

#include "ControlFlow.h"
#include "IRCode.h"
#include "ReachingDefinitions.h"

namespace {

using namespace live_range;
using namespace sparta;

/*
 * Type aliases for disjoint_sets
 */
using Rank = std::unordered_map<Def, size_t>;
using Parent = std::unordered_map<Def, Def>;
using RankPMap = boost::associative_property_map<Rank>;
using ParentPMap = boost::associative_property_map<Parent>;
using DefSets = boost::disjoint_sets<RankPMap, ParentPMap>;

/*
 * Allocates a unique symbolic register for every disjoint set of defs.
 */
class SymRegMapper {
 public:
  SymRegMapper(bool width_aware) : m_width_aware(width_aware) {}

  reg_t make(Def def) {
    if (m_def_to_reg.find(def) == m_def_to_reg.end()) {
      if (m_width_aware) {
        m_def_to_reg[def] = m_next_symreg;
        m_next_symreg += def->dest_is_wide() ? 2 : 1;
      } else {
        m_def_to_reg[def] = m_next_symreg++;
      }
    }
    return m_def_to_reg.at(def);
  }

  reg_t at(Def def) { return m_def_to_reg.at(def); }

  reg_t regs_size() const { return m_next_symreg; }

 private:
  bool m_width_aware;
  reg_t m_next_symreg{0};
  std::unordered_map<Def, reg_t> m_def_to_reg;
};

using UDChains = std::unordered_map<Use, PatriciaTreeSet<Def>>;

/*
 * Put all defs with a use in common into the same set.
 */
void unify_defs(const UDChains& chains, DefSets* def_sets) {
  for (const auto& chain : chains) {
    auto& defs = chain.second;
    auto it = defs.begin();
    Def first = *it;
    auto end = defs.end();
    for (++it; it != end; ++it) {
      def_sets->union_set(first, *it);
    }
  }
}

UDChains calculate_ud_chains(const cfg::ControlFlowGraph& cfg) {
  reaching_defs::FixpointIterator fixpoint_iter{cfg};
  fixpoint_iter.run(reaching_defs::Environment());
  UDChains chains;
  for (cfg::Block* block : cfg.blocks()) {
    reaching_defs::Environment defs_in =
        fixpoint_iter.get_entry_state_at(block);
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src = insn->src(i);
        Use use{insn, src};
        auto defs = defs_in.get(src);
        always_assert_log(!defs.is_top() && defs.size() > 0,
                          "Found use without def when processing [0x%lx]%s",
                          &mie, SHOW(insn));
        chains[use] = defs.elements();
      }
      fixpoint_iter.analyze_instruction(insn, &defs_in);
    }
  }
  return chains;
}

} // namespace

namespace live_range {

bool Use::operator==(const Use& that) const {
  return insn == that.insn && reg == that.reg;
}

void renumber_registers(IRCode* code, bool width_aware) {
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto chains = calculate_ud_chains(cfg);

  Rank rank;
  Parent parent;
  DefSets def_sets((RankPMap(rank)), (ParentPMap(parent)));
  for (const auto& mie : cfg::InstructionIterable(cfg)) {
    if (mie.insn->dests_size()) {
      def_sets.make_set(mie.insn);
    }
  }
  unify_defs(chains, &def_sets);
  SymRegMapper sym_reg_mapper(width_aware);
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      auto sym_reg = sym_reg_mapper.make(def_sets.find_set(insn));
      insn->set_dest(sym_reg);
    }
  }
  for (auto& mie : cfg::InstructionIterable(cfg)) {
    auto insn = mie.insn;
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      auto& defs = chains.at(Use{insn, insn->src(i)});
      insn->set_src(i, sym_reg_mapper.at(def_sets.find_set(*defs.begin())));
    }
  }
  cfg.set_registers_size(sym_reg_mapper.regs_size());
  code->clear_cfg();
}

} // namespace live_range
