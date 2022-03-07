/*
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
#include "StlUtil.h"
#include "Trace.h"

namespace {

// Return true if this block is a leaf.
// Any block that is not part of the if/switch tree is considered a leaf.
bool is_leaf(cfg::ControlFlowGraph* cfg, cfg::Block* b, reg_t reg) {

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
    if (!(opcode::is_a_literal_const(op) || opcode::is_branch(op))) {
      // non-leaf nodes only have const and branch instructions
      return true;
    }
    if (insn->has_dest() &&
        (insn->dest() == reg ||
         (insn->dest_is_wide() && insn->dest() + 1 == reg))) {
      // Overwriting the switching reg marks the end of the switch construct
      return true;
    }
  }

  auto last_insn = last->insn;
  auto last_op = last_insn->opcode();
  if (opcode::is_branch(last_op) &&
      SwitchEquivFinder::has_src(last_insn, reg)) {
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
      [&b](const std::pair<reg_t, IRInstruction*>& it) {
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
bool SwitchEquivFinder::has_src(IRInstruction* insn, reg_t reg) {
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
    reg_t switching_reg,
    uint32_t leaf_duplication_threshold)
    : m_cfg(cfg),
      m_root_branch(root_branch),
      m_switching_reg(switching_reg),
      m_leaf_duplication_threshold(leaf_duplication_threshold) {

  {
    // make sure the input is well-formed
    auto insn = m_root_branch->insn;
    auto op = insn->opcode();
    always_assert(opcode::is_branch(op));
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
  std::unordered_set<cfg::Block*> non_leaves;
  std::function<bool(cfg::Block*, InstructionSet)> recurse;
  std::vector<std::pair<cfg::Edge*, cfg::Block*>> edges_to_move;
  recurse = [&](cfg::Block* b, const InstructionSet& loads) {
    // `loads` represents the state of the registers after evaluating `b`.
    for (cfg::Edge* succ : b->succs()) {
      cfg::Block* next = succ->target();

      uint16_t count = ++m_visit_count[next];
      if (count > next->preds().size()) {
        // Infinite loop. Bail
        TRACE(SWITCH_EQUIV, 2, "Failure Reason: Detected loop");
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
            if (next->num_opcodes() < m_leaf_duplication_threshold) {
              // A switch cannot represent this control flow graph unless we
              // duplicate this leaf. See the comment on
              // m_leaf_duplication_theshold for more details.
              always_assert(m_cfg->editable());
              cfg::Block* copy = m_cfg->duplicate_block(next);
              edges_to_move.emplace_back(succ, copy);
              m_extra_loads.emplace(copy, loads);
            } else {
              TRACE(SWITCH_EQUIV, 2, "Failure Reason: divergent entry states");
              TRACE(SWITCH_EQUIV, 3, "B%zu in %s", next->id(), SHOW(*m_cfg));
              return false;
            }
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
          if (opcode::is_a_literal_const(op)) {
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

  const auto& bail = [&leaves, this, &edges_to_move]() {
    // While traversing the CFG, we may have duplicated case blocks (see the
    // comment on m_leaf_duplication_threshold for more details). If we did not
    // successfully find a switch equivalent here, we need to remove those
    // blocks.
    std::vector<cfg::Block*> blocks_to_remove;
    for (const auto& pair : edges_to_move) {
      cfg::Block* copy = pair.second;
      blocks_to_remove.push_back(copy);
    }
    m_cfg->remove_blocks(blocks_to_remove);
    leaves.clear();
    return leaves;
  };

  bool success = recurse(m_root_branch.block(), {});
  if (!success) {
    return bail();
  }

  normalize_extra_loads(non_leaves);

  if (!m_extra_loads.empty()) {
    // Make sure there are no other ways to reach the leaf nodes. If there were
    // other ways to reach them, m_extra_loads would be incorrect.
    for (const auto& block_and_count : m_visit_count) {
      cfg::Block* b = block_and_count.first;
      uint16_t count = block_and_count.second;
      if (b->preds().size() > count) {
        TRACE(SWITCH_EQUIV, 2,
              "Failure Reason: Additional ways to reach blocks");
        TRACE(SWITCH_EQUIV, 3,
              "  B%zu has %zu preds but was hit %d times in \n%s", b->id(),
              b->preds().size(), count, SHOW(*m_cfg));
        return bail();
      }
    }
  }

  success = move_edges(edges_to_move);
  if (!success) {
    return bail();
  }

  if (leaves.empty()) {
    TRACE(SWITCH_EQUIV, 2, "Failure Reason: No leaves found");
    TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
  }
  return leaves;
}

bool SwitchEquivFinder::move_edges(
    const std::vector<std::pair<cfg::Edge*, cfg::Block*>>& edges_to_move) {
  for (const auto& pair : edges_to_move) {
    cfg::Edge* edge = pair.first;
    cfg::Block* orig = edge->target();
    for (cfg::Edge* orig_succ : orig->succs()) {
      always_assert_log(orig_succ != nullptr, "B%zu in %s", orig->id(),
                        SHOW(*m_cfg));
      if (orig_succ->type() == cfg::EDGE_GOTO &&
          orig_succ->target()->starts_with_move_result()) {
        // Two blocks can't share a single move-result-psuedo
        TRACE(SWITCH_EQUIV, 2,
              "Failure Reason: Can't share move-result-pseudo");
        TRACE(SWITCH_EQUIV, 3, "%s", SHOW(*m_cfg));
        return false;
      }
    }
  }
  std::vector<cfg::Block*> blocks_to_remove;
  for (const auto& pair : edges_to_move) {
    cfg::Edge* edge = pair.first;
    cfg::Block* orig = edge->target();
    cfg::Block* copy = pair.second;
    const auto& copy_loads = m_extra_loads.find(copy);
    const auto& orig_loads = m_extra_loads.find(orig);
    const auto& have_copy_loads = copy_loads != m_extra_loads.end();
    const auto& have_orig_loads = orig_loads != m_extra_loads.end();
    bool just_one_null = have_copy_loads != have_orig_loads;
    bool both_null = !have_copy_loads && !have_orig_loads;
    if (!just_one_null &&
        (both_null || ::equals(copy_loads->second, orig_loads->second))) {
      // When we normalized the extra loads, the copy and original may have
      // converged to the same state. We don't need the duplicate block anymore
      // in this case.
      blocks_to_remove.push_back(copy);
      continue;
    }
    // copy on purpose so we can alter in the loop
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto orig_succs = orig->succs();
    for (cfg::Edge* orig_succ : orig_succs) {
      cfg::Edge* copy_succ = new cfg::Edge(*orig_succ);
      m_cfg->add_edge(copy_succ);
      m_cfg->set_edge_source(copy_succ, copy);
    }
    m_cfg->set_edge_target(edge, copy);
  }
  m_cfg->remove_blocks(blocks_to_remove);
  return true;
}

// Before this function, m_extra_loads is overly broad
// * Remove loads that are never used outside the if-else chain blocks
// * Remove empty lists of loads from the map (possibly emptying the map)
void SwitchEquivFinder::normalize_extra_loads(
    const std::unordered_set<cfg::Block*>& non_leaves) {

  // collect the extra loads
  std::unordered_set<IRInstruction*> extra_loads;
  for (const auto& non_leaf : non_leaves) {
    for (const auto& mie : InstructionIterable(non_leaf)) {
      if (opcode::is_a_literal_const(mie.insn->opcode())) {
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
    if (defs_in.is_bottom()) {
      continue;
    }
    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src = insn->src(i);
        const auto& defs = defs_in.get(src);
        always_assert_log(!defs.is_top(), "Undefined register v%u", src);
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
    std20::erase_if(insns, [&used_defs](auto& p) {
      return used_defs.count(p.second) == 0;
    });
  }

  // Remove empty instruction lists from `m_extra_loads` (possibly emptying it)
  std20::erase_if(m_extra_loads, [](auto& p) { return p.second.empty(); });
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
      TRACE(SWITCH_EQUIV, 2, "Failure Reason: Divergent key to block mapping");
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

std::vector<cfg::Block*> SwitchEquivFinder::visited_blocks() const {
  std::vector<cfg::Block*> result;
  result.reserve(1 + m_visit_count.size());
  result.emplace_back(m_root_branch.block());
  for (auto entry : m_visit_count) {
    result.emplace_back(entry.first);
  }
  return result;
}
