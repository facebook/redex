/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Interference.h"

#include "ControlFlow.h"
#include "DexUtil.h"
#include "FixpointIterators.h"
#include "Transform.h"

namespace {

using namespace regalloc;
using namespace regalloc::interference;
using namespace std::placeholders;

class LivenessFixpointIterator final
    : public MonotonicFixpointIterator<Block*, LivenessDomain> {
 public:
  using NodeId = Block*;

  LivenessFixpointIterator(NodeId exit_block)
      : MonotonicFixpointIterator(exit_block,
                                  std::bind(&Block::preds, _1),
                                  std::bind(&Block::succs, _1)) {}

  void analyze_node(const NodeId& block,
                    LivenessDomain* current_state) const override {
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type == MFLOW_OPCODE) {
        analyze_instruction(it->insn, current_state);
      }
    }
  }

  LivenessDomain analyze_edge(
      const NodeId& /* source_block */,
      const NodeId& /* target_block */,
      const LivenessDomain& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_instruction(const IRInstruction* insn,
                           LivenessDomain* current_state) const {
    if (insn->dests_size()) {
      current_state->remove(insn->dest());
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      current_state->add(insn->src(i));
    }
    always_assert(!opcode::has_range(insn->opcode()));
  }

  LivenessDomain get_live_in_vars_at(const NodeId& block) const {
    return get_exit_state_at(block);
  }

  LivenessDomain get_live_out_vars_at(const NodeId& block) const {
    return get_entry_state_at(block);
  }
};

} // namespace

namespace regalloc {

namespace interference {

using namespace impl;

/*
 * We determine a node's colorability using equation E.3 in [Smith00] for
 * registers of varying width in an unaligned architecture.
 *
 * Let w(n) be the width of a node n. E.3 says that n is colorable if the
 * following inequality holds:
 *
 *   ( ∑ ⌈w(j)/w(n)⌉ ) < ⌈register_frame_size / (2 * w(n) + 1)⌉
 *
 * where we take the summation over all nodes j that are adjacent to n. Note
 * that if w(x) = 1 for all nodes x, this reduces to Chaitin's criterion of
 * degree(n) < register_frame_size.
 *
 * To evaluate that inequality in our implementation, we treat ⌈w(j)/w(n)⌉ as
 * an "edge weight" -- but note that even though the edges are undirected,
 * in general edge_weight(u, v) != edge_weight(v, u).
 *
 * The LHS of the inequality is what we call the "node weight" in our
 * implementation -- it is the sum of the weights of its edges.
 */

static uint32_t edge_weight(const Node& u, const Node& v) {
  return div_ceil(v.width(), u.width());
}

void Graph::add_edge(reg_t u, reg_t v) {
  if (u == v || m_adj_matrix.find({u, v}) != m_adj_matrix.end()) {
    return;
  }
  m_adj_matrix.emplace(u, v);
  m_adj_matrix.emplace(v, u);
  auto& u_node = m_nodes.at(u);
  auto& v_node = m_nodes.at(v);
  u_node.m_adjacent.push_back(v);
  v_node.m_adjacent.push_back(u);
  u_node.m_weight += edge_weight(u_node, v_node);
  v_node.m_weight += edge_weight(v_node, u_node);
}

uint32_t Node::colorable_limit() const {
  return div_ceil(max_vreg() + 1, 2 * width() - 1);
}

bool Node::definitely_colorable() const { return weight() < colorable_limit(); }

const Node& Graph::get_node(reg_t v) const { return m_nodes.at(v); }

void Graph::combine(reg_t u, reg_t v) {
  auto& u_node = m_nodes.at(u);
  auto& v_node = m_nodes.at(v);
  for (auto t : v_node.adjacent()) {
    auto& t_node = m_nodes.at(t);
    if (!t_node.is_active()) {
      continue;
    }
    add_edge(u, t);
  }
  u_node.m_weight -= edge_weight(u_node, v_node);
  v_node.m_weight -= edge_weight(v_node, u_node);
  u_node.m_max_vreg = std::min(u_node.m_max_vreg, v_node.m_max_vreg);
  u_node.m_type_domain.meet_with(v_node.m_type_domain);
  u_node.m_props |= v_node.m_props;
  v_node.m_props.reset(Node::ACTIVE);
}

void Graph::remove_node(reg_t u) {
  auto& u_node = m_nodes.at(u);
  for (auto v : u_node.adjacent()) {
    auto& v_node = m_nodes.at(v);
    if (!v_node.is_active()) {
      continue;
    }
    v_node.m_weight -= edge_weight(v_node, u_node);
  }
  u_node.m_props.reset(Node::ACTIVE);
}

void GraphBuilder::update_node_constraints(const IRInstruction* insn,
                                           const RangeSet& range_set,
                                           Graph* graph) {
  auto op = insn->opcode();
  if (insn->dests_size()) {
    auto dest = insn->dest();
    auto& node = graph->m_nodes[dest];
    if (opcode::is_load_param(op)) {
      node.m_props.set(Node::PARAM);
    }
    node.m_type_domain.meet_with(RegisterTypeDomain(dest_reg_type(insn)));
    node.m_max_vreg =
        std::min(node.m_max_vreg, max_unsigned_value(insn->dest_bit_width()));
  }

  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto src = insn->src(i);
    auto& node = graph->m_nodes[src];
    auto type = src_reg_type(insn, i);
    node.m_type_domain.meet_with(RegisterTypeDomain(type));
    reg_t max_vreg;
    if (range_set.contains(insn)) {
      max_vreg = max_unsigned_value(16);
      node.m_props.set(Node::RANGE);
    } else if (opcode::has_range_form(op) && insn->srcs_size() == 1) {
      // An `invoke {v0}` opcode can always be rewritten as `invoke/range {v0}`
      max_vreg = max_unsigned_value(16);
    } else {
      max_vreg = max_unsigned_value(insn->src_bit_width(i));
      if (is_invoke(op) && type == RegisterType::WIDE) {
        // invoke instructions need to address both pairs of a wide register in
        // their denormalized form. We are dealing with the normalized form
        // here, so we need to reserve one register for denormalization. I.e.
        // `invoke-static {v14} LFoo.a(J)` will expand into
        // `invoke-static {v14, v15} LFoo.a(J)` after denormalization.
        --max_vreg;
      }
    }
    node.m_max_vreg = std::min(node.m_max_vreg, max_vreg);
  }
}

static IRInstruction* find_check_cast(const MethodItemEntry& mie) {
  always_assert(mie.type == MFLOW_FALLTHROUGH);
  if (mie.throwing_mie != nullptr &&
      mie.throwing_mie->insn->opcode() == OPCODE_CHECK_CAST) {
    return mie.throwing_mie->insn;
  } else {
    return nullptr;
  }
}

/*
 * Build the interference graph by adding edges between nodes that are
 * simultaneously live.
 *
 * check-cast instructions have to be handled specially. They are represented
 * with both a dest and a src in our IR. However, in actual Dex bytecode, it
 * only takes a single operand which acts as both src and dest. So when
 * converting IR to Dex bytecode, we need to insert a move instruction if the
 * src and dest operands differ. We must insert the move before, not after, the
 * check-cast. Suppose we did not:
 *
 *        IR                  |           Dex
 *   sget-object v0 LFoo;     |  sget-object v0 LFoo;
 *   check-cast v1 v0 LBar;   |  check-cast v0 LBar;
 *                            |  move-object v1 v0
 *   invoke-static v0 LFoo.a; |  invoke-static v0 LFoo.a; // v0 is of type Bar!
 *
 * However, inserting before the check-cast is tricky to get right. If the
 * check-cast is in a try region, we must be careful to not clobber other
 * live registers. For example, if we had some IRCode like
 *
 *   B0:
 *     load-param v1 Ljava/lang/Object;
 *     TRY_START
 *     const/4 v0 123
 *   B1:
 *     check-cast v0, v1 LFoo;
 *     return v0
 *     TRY_END
 *   B2:
 *     CATCH
 *     // handle failure of check-cast
 *     // Note that v0 has the value of 123 here because the check-cast failed
 *     add-int v0, v0, v0
 *
 * Inserting the move before the check-cast would cause v0 to have an object
 * (instead of integer) type inside the exception handler.
 *
 * The solution is to have the interference graph make check-cast's dest
 * register interfere with the live registers in both B0 and B1, so that when
 * the move gets inserted, it does not clobber any live registers.
 */
Graph GraphBuilder::build(IRCode* code,
                          reg_t initial_regs,
                          const RangeSet& range_set) {
  Graph graph;
  for (const auto& mie : InstructionIterable(code)) {
    GraphBuilder::update_node_constraints(mie.insn, range_set, &graph);
  }
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code->get_registers_size()));
  for (Block* block : cfg.blocks()) {
    LivenessDomain live_out = fixpoint_iter.get_live_out_vars_at(block);
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type == MFLOW_FALLTHROUGH) {
        auto check_cast = find_check_cast(*it);
        if (check_cast != nullptr) {
          for (auto reg : live_out.elements()) {
            graph.add_edge(check_cast->dest(), reg);
          }
        }
        continue;
      } else if (it->type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = it->insn;
      auto op = insn->opcode();
      if (opcode::has_range_form(op)) {
        graph.m_range_liveness.emplace(insn, live_out);
      }
      if (insn->dests_size()) {
        for (auto reg : live_out.elements()) {
          // We don't want to add interference edges between the src and dest
          // of a move instruction so that we have the option of coalescing
          // those live ranges later. However, our current implementation
          // doesn't work if we leave out the interference edges of move-wide
          // instructions. If we have `move-wide s0, s1` with both s0 and s1
          // live-out, and we don't add an edge between s0 and s1, we may end
          // up with an allocation like `move-wide v0, v1` which is invalid
          // since v0 is clobbering v1. So we add edges for move-wides, but
          // that means that they can never be coalesced. FIXME
          if (is_move(op) && !insn->is_wide() && reg == insn->src(0)) {
            continue;
          }
          graph.add_edge(insn->dest(), reg);
        }
      }
      fixpoint_iter.analyze_instruction(it->insn, &live_out);
    }
  }
  for (auto& pair : graph.nodes()) {
    auto reg = pair.first;
    auto& node = pair.second;
    if (reg >= initial_regs) {
      node.m_props.set(Node::SPILL);
    }
    assert(!node.m_type_domain.is_bottom());
  }
  return graph;
}

std::ostream& Graph::write_dot_format(std::ostream& o) const {
  o << "graph {\n";
  for (const auto& pair : nodes()) {
    auto reg = pair.first;
    auto& node = pair.second;
    o << reg << "[label=\"" << reg << " (" << node.weight() << ")\"]"
      << "\n";
    for (auto adj : node.adjacent()) {
      if (pair.first < adj) {
        o << reg << " -- " << adj << "\n";
      }
    }
  }
  o << "}\n";
  return o;
}

void GraphBuilder::make_node(Graph* graph,
                             reg_t r,
                             RegisterType type,
                             reg_t max_vreg) {
  always_assert(graph->m_nodes.find(r) == graph->m_nodes.end());
  graph->m_nodes[r].m_type_domain.meet_with(RegisterTypeDomain(type));
  graph->m_nodes[r].m_max_vreg = max_vreg;
}

void GraphBuilder::add_edge(Graph* graph, reg_t u, reg_t v) {
  graph->add_edge(u, v);
}

} // namespace interference

} // namespace regalloc
