/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SwitchEquivFinder.h"

#include <queue>
#include <vector>

#include "ConstantPropagationAnalysis.h"
#include "ReachingDefinitions.h"

namespace {

// Return true if this block is a leaf.
// Any block that is not part of the if/switch tree is considered a leaf.
bool is_leaf(cfg::ControlFlowGraph* cfg, cfg::Block* b, uint16_t reg) {

  // non-leaf nodes only have GOTO and BRANCH outgoing edges
  if (cfg->get_succ_edge_if(b, [](const cfg::Edge* e) {
        return e->type() == cfg::EDGE_GHOST || e->type() == cfg::EDGE_THROW;
      }) != nullptr) {
    return true;
  }

  const auto& last = b->get_last_insn();
  if (last == b->end()) {
    // No instructions in this block => can't be part of the switching logic =>
    // must be a leaf
    return true;
  }
  for (const auto& mie : InstructionIterable(b)) {
    auto insn = mie.insn;
    auto op = insn->opcode();
    if (!(is_literal_const(op) || is_branch(op))) {
      // non-leaf nodes only have const and branch instructions
      return true;
    }
    if (insn->dests_size() &&
        (insn->dest() == reg ||
         (insn->dest_is_wide() && insn->dest() + 1 == reg))) {
      // Overwriting the switching reg marks the end of the switch construct
      return true;
    }
  }

  auto last_insn = last->insn;
  auto last_op = last_insn->opcode();
  if (is_branch(last_op) && SwitchEquivFinder::has_src(last_insn, reg)) {
    // The only non-leaf block is one that branches on the switching reg
    return false;
  }

  // Any other block must be a leaf
  return true;
}

bool equals(const SwitchEquivFinder::InstructionSet& a,
            const SwitchEquivFinder::InstructionSet& b) {

  if (a.size() != b.size()) {
    return false;
  }
  const auto& has_equivalent =
      [&b](const std::pair<uint16_t, IRInstruction*>& it) {
        const auto& search = b.find(it.first);
        if (search != b.end()) {
          bool just_one_null =
              (search->second == nullptr) != (it.second == nullptr);
          if (just_one_null) {
            return false;
          }
          bool both_null = search->second == nullptr && it.second == nullptr;
          if (both_null || *search->second == *it.second) {
            return true;
          }
        }
        return false;
      };

  for (auto it = a.begin(); it != a.end(); ++it) {
    if (!has_equivalent(*it)) {
      return false;
    }
  }
  return true;
}

} // namespace

namespace cp = constant_propagation;

// Return true if any of the sources of `insn` are `reg`.
bool SwitchEquivFinder::has_src(IRInstruction* insn, uint16_t reg) {
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    if (insn->src(i) == reg) {
      return true;
    }
  }
  return false;
}

SwitchEquivFinder::SwitchEquivFinder(
    cfg::ControlFlowGraph* cfg,
    const cfg::InstructionIterator& root_branch,
    uint16_t switching_reg)
    : m_cfg(cfg), m_root_branch(root_branch), m_switching_reg(switching_reg) {

  {
    // make sure the input is well-formed
    auto insn = m_root_branch->insn;
    auto op = insn->opcode();
    always_assert(is_branch(op));
    always_assert(has_src(insn, m_switching_reg));
  }

  const auto& leaves = find_leaves();
  if (leaves.empty()) {
    m_extra_loads.clear();
    m_success = false;
    return;
  }
  find_case_keys(leaves);
}

// Starting from the branch instruction, find all reachable branch
// instructions (with no intervening leaf blocks) that also have `reg` as a
// source (and without `reg` being overwritten)
//
// While we're searching for the leaf blocks, keep track of any constant loads
// that occur between the root branch and the leaf block. Put those in
// `m_extra_loads`.
std::vector<cfg::Edge*> SwitchEquivFinder::find_leaves() {
  std::vector<cfg::Edge*> leaves;

  // Traverse the tree in an depth first order so that the extra loads are
  // tracked in the same order that they will be executed at runtime
  std::unordered_map<cfg::Block*, uint16_t> visit_count;
  std::unordered_set<cfg::Block*> non_leaves;
  std::function<bool(cfg::Block*, InstructionSet)> recurse;
  recurse = [&](cfg::Block* b, const InstructionSet& loads) {
    // `loads` represents the state of the registers after evaluating `b`.
    for (cfg::Edge* succ : b->succs()) {
      cfg::Block* next = succ->target();

      uint16_t count = ++visit_count[next];
      if (count > next->preds().size()) {
        // Infinite loop. Bail
        TRACE(SWITCH_EQUIV, 2, "Failure Reason: Detected loop\n");
        TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
        return false;
      }

      if (is_leaf(m_cfg, next, m_switching_reg)) {
        leaves.push_back(succ);
        const auto& pair = m_extra_loads.emplace(next, loads);
        bool already_there = !pair.second;
        if (already_there) {
          // There are multiple ways to reach this leaf. Make sure the extra
          // loads are consistent.
          const auto& it = pair.first;
          const InstructionSet& existing_loads = it->second;
          if (!::equals(existing_loads, loads)) {
            TRACE(SWITCH_EQUIV, 2, "Failure Reason: divergent entry states\n");
            TRACE(SWITCH_EQUIV, 3, "B%d in %s", next->id(), SHOW(*m_cfg));
            return false;
          }
        }
      } else {
        non_leaves.insert(next);
        boost::optional<InstructionSet> next_loads;
        for (const auto& mie : InstructionIterable(next)) {
          // A chain of if-else blocks loads constants into register to do the
          // comparisons, however, the leaf blocks may also use those registers,
          // so this function finds any loads that occur in non-leaf blocks that
          // lead to `leaf`.
          auto insn = mie.insn;
          auto op = insn->opcode();
          if (is_literal_const(op)) {
            if (next_loads == boost::none) {
              // Copy loads here because we only want these loads to propagate
              // to successors of `next`, not any other successors of `b`
              next_loads = loads;
            }
            // Overwrite any previous mapping for this dest register.
            (*next_loads)[insn->dest()] = insn;
            if (insn->dest_is_wide()) {
              // And don't forget to clear out the upper register of wide loads.
              (*next_loads)[insn->dest() + 1] = nullptr;
            }
          }
        }
        bool success = false;
        if (next_loads != boost::none) {
          success = recurse(next, *next_loads);
        } else {
          success = recurse(next, loads);
        }
        if (!success) {
          return false;
        }
      }
    }
    return true;
  };

  bool success = recurse(m_root_branch.block(), {});
  if (!success) {
    leaves.clear();
    return leaves;
  }

  normalize_extra_loads(non_leaves);

  if (!m_extra_loads.empty()) {
    // Make sure there are no other ways to reach the leaf nodes. If there were
    // other ways to reach them, m_extra_loads would be incorrect.
    for (const auto& block_and_count : visit_count) {
      cfg::Block* b = block_and_count.first;
      uint16_t count = block_and_count.second;
      if (b->preds().size() != count) {
        TRACE(SWITCH_EQUIV,
              2,
              "Failure Reason: Additional ways to reach blocks\n"
              "  B%d has %d preds but was hit %d times\n",
              b->id(),
              b->preds().size(),
              count);
        TRACE(SWITCH_EQUIV,
              3,
              "B%d -> [] size %d ",
              m_extra_loads.begin()->first->id(),
              m_extra_loads.begin()->second.size());
        TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
        leaves.clear();
        return leaves;
      }
    }
  }

  if (leaves.empty()) {
    TRACE(SWITCH_EQUIV, 2, "Failure Reason: No leaves found\n");
    TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
  }
  return leaves;
}

// Before this function, m_extra_loads is overly broad
// * Remove loads that are never used outside the if-else chain blocks
// * Remove empty lists of loads from the map (possibly emptying the map)
void SwitchEquivFinder::normalize_extra_loads(
    std::unordered_set<cfg::Block*> non_leaves) {

  // collect the extra loads
  std::unordered_set<IRInstruction*> extra_loads;
  for (const auto& non_leaf : non_leaves) {
    for (const auto& mie : InstructionIterable(non_leaf)) {
      if (is_literal_const(mie.insn->opcode())) {
        extra_loads.insert(mie.insn);
      }
    }
  }

  // Use ReachingDefinitions to find the loads that are used outside the if-else
  // chain blocks
  std::unordered_set<IRInstruction*> used_defs;
  reaching_defs::FixpointIterator fixpoint_iter(*m_cfg);
  fixpoint_iter.run(reaching_defs::Environment());
  for (cfg::Block* block : m_cfg->blocks()) {
    if (non_leaves.count(block)) {
      continue;
    }
    reaching_defs::Environment defs_in =
        fixpoint_iter.get_entry_state_at(block);
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src = insn->src(i);
        auto defs = defs_in.get(src);
        if (defs.is_top()) {
          // Probably an unreachable block. Ignore and let the IRTypeChecker
          // find real problems
          continue;
        }
        for (IRInstruction* def : defs.elements()) {
          if (extra_loads.count(def)) {
            used_defs.insert(def);
          }
        }
      }
      fixpoint_iter.analyze_instruction(insn, &defs_in);
    }
  }

  // Remove loads that aren't used outside the if-else chain blocks
  for (auto& block_and_insns : m_extra_loads) {
    InstructionSet& insns = block_and_insns.second;
    for (auto it = insns.begin(); it != insns.end();) {
      if (used_defs.count(it->second) == 0) {
        it = insns.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Remove empty instruction lists from `m_extra_loads` (possibly emptying it)
  for (auto it = m_extra_loads.begin(); it != m_extra_loads.end();) {
    if (it->second.size() == 0) {
      it = m_extra_loads.erase(it);
    } else {
      ++it;
    }
  }
}

// Use a sparta analysis to find the value of reg at the beginning of each leaf
// block
void SwitchEquivFinder::find_case_keys(const std::vector<cfg::Edge*>& leaves) {
  // We use the fixpoint iterator to infer the values of registers at different
  // points in the program. Especially `m_switching_reg`.
  cp::intraprocedural::FixpointIterator fixpoint(
      *m_cfg, cp::ConstantPrimitiveAnalyzer());
  fixpoint.run(ConstantEnvironment());

  // return true on success
  // return false on failure (there was a conflicting entry already in the map)
  const auto& insert = [this](boost::optional<int32_t> c, cfg::Block* b) {
    const auto& pair = m_key_to_case.emplace(c, b);
    const auto& it = pair.first;
    bool already_there = !pair.second;
    if (already_there && it->second != b) {
      TRACE(
          SWITCH_EQUIV, 2, "Failure Reason: Divergent key to block mapping\n");
      TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
      return false;
    }
    return true;
  };

  // returns the value of `m_switching_reg` if the leaf is reached via this edge
  const auto& get_case_key =
      [this, &fixpoint](cfg::Edge* edge_to_leaf) -> boost::optional<int32_t> {
    // Get the inferred value of m_switching_reg at the end of `edge_to_leaf`
    // but before the beginning of the leaf block because we would lose the
    // information by merging all the incoming edges.
    auto env = fixpoint.get_exit_state_at(edge_to_leaf->src());
    env = fixpoint.analyze_edge(edge_to_leaf, env);
    const auto& case_key = env.get<SignedConstantDomain>(m_switching_reg);
    if (case_key.is_top() || case_key.get_constant() == boost::none) {
      // boost::none represents the fallthrough block
      return boost::none;
    } else {
      // It's safe to cast down to 32 bits because long values can't be used in
      // switch statements.
      return static_cast<int32_t>(*case_key.get_constant());
    }
  };

  for (cfg::Edge* edge_to_leaf : leaves) {
    const auto& case_key = get_case_key(edge_to_leaf);
    bool success = insert(case_key, edge_to_leaf->target());
    if (!success) {
      // If we didn't insert into result for this leaf node, abort the entire
      // operation because we don't want to present incomplete information about
      // the possible successors
      m_key_to_case.clear();
      m_extra_loads.clear();
      m_success = false;
      return;
    }
  }
  m_success = true;
}
