/*
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
#include "ScopedCFG.h"

namespace {

using namespace live_range;

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
  explicit SymRegMapper(bool width_aware) : m_width_aware(width_aware) {}

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

/*
 * Put all defs with a use in common into the same set.
 */
void unify_defs(const UseDefChains& chains, DefSets* def_sets) {
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

} // namespace

namespace live_range {

bool Use::operator==(const Use& that) const {
  return insn == that.insn && src_index == that.src_index;
}

Chains::Chains(const cfg::ControlFlowGraph& cfg) : m_cfg(cfg), m_fp_iter(cfg) {
  m_fp_iter.run(reaching_defs::Environment());
}

template <typename Fn>
void Chains::replay_analysis_with_callback(Fn f) const {
  for (cfg::Block* block : m_cfg.blocks()) {
    auto defs_in = m_fp_iter.get_entry_state_at(block);
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      for (src_index_t i = 0; i < insn->srcs_size(); ++i) {
        Use use{insn, i};
        auto src = insn->src(i);
        auto defs = defs_in.get(src);
        always_assert_log(!defs.is_top() && defs.size() > 0,
                          "Found use without def when processing [0x%lx]%s",
                          &mie, SHOW(insn));
        f(use, defs);
      }
      m_fp_iter.analyze_instruction(insn, &defs_in);
    }
  }
}

UseDefChains Chains::get_use_def_chains() const {
  UseDefChains chains;
  replay_analysis_with_callback(
      [&chains](const Use& use, const reaching_defs::Domain& defs) {
        chains[use] = defs.elements();
      });
  return chains;
}

DefUseChains Chains::get_def_use_chains() const {
  DefUseChains chains;
  replay_analysis_with_callback(
      [&chains](const Use& use, const reaching_defs::Domain& defs) {
        for (auto def : defs.elements()) {
          chains[def].emplace(use);
        }
      });
  return chains;
}

void renumber_registers(IRCode* code, bool width_aware) {
  cfg::ScopedCFG cfg(code);

  auto ud_chains = Chains(*cfg).get_use_def_chains();

  Rank rank;
  Parent parent;
  DefSets def_sets((RankPMap(rank)), (ParentPMap(parent)));
  for (const auto& mie : cfg::InstructionIterable(*cfg)) {
    if (mie.insn->has_dest()) {
      def_sets.make_set(mie.insn);
    }
  }
  unify_defs(ud_chains, &def_sets);
  SymRegMapper sym_reg_mapper(width_aware);
  for (auto& mie : cfg::InstructionIterable(*cfg)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      auto sym_reg = sym_reg_mapper.make(def_sets.find_set(insn));
      insn->set_dest(sym_reg);
    }
  }
  for (auto& mie : cfg::InstructionIterable(*cfg)) {
    auto insn = mie.insn;
    for (src_index_t i = 0; i < insn->srcs_size(); ++i) {
      auto& defs = ud_chains.at(Use{insn, i});
      insn->set_src(i, sym_reg_mapper.at(def_sets.find_set(*defs.begin())));
    }
  }
  cfg->set_registers_size(sym_reg_mapper.regs_size());
}

} // namespace live_range
