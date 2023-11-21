/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include "Show.h"
#include "Timer.h"

namespace {

AccumulatingTimer s_timer("live_range");

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

template <typename Iter, typename Fn>
void replay_analysis_with_callback(const cfg::ControlFlowGraph& cfg,
                                   const Iter& iter,
                                   bool ignore_unreachable,
                                   Fn f) {
  auto timer_scope = s_timer.scope();

  for (cfg::Block* block : cfg.blocks()) {
    auto defs_in = iter.get_entry_state_at(block);
    if (ignore_unreachable && defs_in.is_bottom()) {
      continue;
    }
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      for (src_index_t i = 0; i < insn->srcs_size(); ++i) {
        Use use{insn, i};
        auto src = insn->src(i);
        const auto& defs = defs_in.get(src);
        if (defs.is_top() || defs.empty()) {
          if (iter.has_filter()) {
            continue;
          }
          not_reached_log("Found use without def when processing [0x%p]%s",
                          &mie, SHOW(insn));
        }
        always_assert_log(!defs.is_bottom(),
                          "Found unreachable use when processing [0x%p]%s",
                          &mie, SHOW(insn));
        f(use, defs);
      }
      iter.analyze_instruction(insn, &defs_in);
    }
  }
}

template <typename Iter>
UseDefChains get_use_def_chains_impl(const cfg::ControlFlowGraph& cfg,
                                     const Iter& iter,
                                     bool ignore_unreachable) {
  UseDefChains chains;
  replay_analysis_with_callback(
      cfg, iter, ignore_unreachable,
      [&chains](const Use& use, const reaching_defs::Domain& defs) {
        chains[use] = defs.elements();
      });
  return chains;
}

template <typename Iter>
DefUseChains get_def_use_chains_impl(const cfg::ControlFlowGraph& cfg,
                                     const Iter& iter,
                                     bool ignore_unreachable) {
  DefUseChains chains;
  replay_analysis_with_callback(
      cfg, iter, ignore_unreachable,
      [&chains](const Use& use, const reaching_defs::Domain& defs) {
        for (auto def : defs.elements()) {
          chains[def].emplace(use);
        }
      });
  return chains;
}

} // namespace

namespace live_range {

bool Use::operator==(const Use& that) const {
  return insn == that.insn && src_index == that.src_index;
}

Chains::Chains(const cfg::ControlFlowGraph& cfg,
               bool ignore_unreachable,
               reaching_defs::Filter filter)
    : m_cfg(cfg),
      m_fp_iter(cfg, std::move(filter)),
      m_ignore_unreachable(ignore_unreachable) {
  auto timer_scope = s_timer.scope();
  m_fp_iter.run(reaching_defs::Environment());
}

UseDefChains Chains::get_use_def_chains() const {
  return get_use_def_chains_impl(m_cfg, m_fp_iter, m_ignore_unreachable);
}

DefUseChains Chains::get_def_use_chains() const {
  return get_def_use_chains_impl(m_cfg, m_fp_iter, m_ignore_unreachable);
}

MoveAwareChains::MoveAwareChains(const cfg::ControlFlowGraph& cfg,
                                 bool ignore_unreachable,
                                 reaching_defs::Filter filter)
    : m_cfg(cfg),
      m_fp_iter(cfg, std::move(filter)),
      m_ignore_unreachable(ignore_unreachable) {
  auto timer_scope = s_timer.scope();
  m_fp_iter.run(reaching_defs::Environment());
}

UseDefChains MoveAwareChains::get_use_def_chains() const {
  return get_use_def_chains_impl(m_cfg, m_fp_iter, m_ignore_unreachable);
}

DefUseChains MoveAwareChains::get_def_use_chains() const {
  return get_def_use_chains_impl(m_cfg, m_fp_iter, m_ignore_unreachable);
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
