/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "CallGraph.h"

#include "VirtualScope.h"
#include "Walkers.h"

namespace call_graph {

Edge::Edge(DexMethod* caller, DexMethod* callee, IRList::iterator invoke_it)
    : m_caller(caller), m_callee(callee), m_invoke_it(invoke_it) {}

Graph::Cache::Cache(const Scope& scope, bool include_virtuals) {
  auto non_virtual_vec =
      include_virtuals ? devirtualize(scope) : std::vector<DexMethod*>();
  m_non_virtual = std::unordered_set<const DexMethod*>(non_virtual_vec.begin(),
                                                       non_virtual_vec.end());
}

Graph Graph::make(const Scope& scope, bool include_virtuals) {
  Graph cg;

  // initialize the caches
  Cache cache(scope, include_virtuals);

  // build the Graph in two steps:
  // 1. the edges
  cg.populate_graph(scope, include_virtuals, cache);
  // 2. the roots
  cg.compute_roots(cache);

  return cg;
}

bool Graph::is_definitely_virtual(
    const DexMethod* method,
    const std::unordered_set<const DexMethod*>& non_virtual) const {
  return method != nullptr && method->is_virtual() &&
         non_virtual.count(method) == 0;
}

void Graph::populate_graph(const Scope& scope,
                           bool include_virtuals,
                           Cache& cache) {
  walk::code(scope, [&](DexMethod* caller, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        auto callee = resolve_method(
            insn->get_method(), opcode_to_search(insn), cache.m_resolved_refs);
        if (callee == nullptr ||
            is_definitely_virtual(callee, cache.m_non_virtual)) {
          continue;
        }
        if (callee->is_concrete()) {
          add_edge(caller, callee, code.iterator_to(mie));
        }
      }
    }
  });
}

// Add edges from the single "ghost" entry node to all the "real" entry
// nodes in the graph. We consider a node to be a potential entry point if
// it is virtual or if it is marked by a Proguard keep rule.
void Graph::compute_roots(Cache& cache) {
  for (auto& pair : m_nodes) {
    auto method = pair.first;
    if (is_definitely_virtual(method, cache.m_non_virtual) || root(method)) {
      auto edge = std::make_shared<Edge>(
          nullptr, const_cast<DexMethod*>(method), IRList::iterator());
      m_entry.m_successors.emplace(edge);
      pair.second.m_predecessors.emplace(edge);
    }
  }
}

Node& Graph::make_node(DexMethod* m) {
  auto it = m_nodes.find(m);
  if (it != m_nodes.end()) {
    return it->second;
  }
  m_nodes.emplace(m, Node(m));
  return m_nodes.at(m);
}

void Graph::add_edge(DexMethod* caller,
                     DexMethod* callee,
                     IRList::iterator invoke_it) {
  auto edge = std::make_shared<Edge>(caller, callee, invoke_it);
  make_node(caller).m_successors.emplace(edge);
  make_node(callee).m_predecessors.emplace(edge);
}

CompleteGraph CompleteGraph::make(const Scope& scope, bool include_virtuals) {
  CompleteGraph cg;

  // initialize the caches
  Cache cache(scope, include_virtuals);

  // build the Graph in two steps:
  // 1. the edges
  cg.populate_graph(scope, include_virtuals, cache);
  // 2. the roots
  cg.compute_roots(cache);

  return cg;
}

// Add all the edges from callers to callee.
// If the callee can be resolved, use it.
// Otherwise, use the unresolved method.
void CompleteGraph::populate_graph(const Scope& scope,
                                   bool include_virtuals,
                                   Cache& cache) {
  TRACE(MORTIROLO, 1, "----- Called subclass\n");

  walk::code(scope, [&](DexMethod* caller, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        const auto callee = resolve_method(
            insn->get_method(), opcode_to_search(insn), cache.m_resolved_refs);
        if (callee != nullptr) {
          add_edge(caller, callee, code.iterator_to(mie));
        } else {
          // add the edge to the unresolved method
          DexMethod* callee_unresolved =
              static_cast<DexMethod*>(insn->get_method());
          add_edge(caller, callee_unresolved, code.iterator_to(mie));
        }
      }
    }
  });
}

void CompleteGraph::compute_roots(Cache& cache) {
  for (auto& pair : m_nodes) {
    auto method = pair.first;
    // TODO: change it to use dominators and SCC instead
    auto preds = make_node(method).m_predecessors.size();
    if (preds == 0 || is_definitely_virtual(method, cache.m_non_virtual) ||
        root(method)) {
      auto edge = std::make_shared<Edge>(
          nullptr, const_cast<DexMethod*>(method), IRList::iterator());
      m_entry.m_successors.emplace(edge);
      pair.second.m_predecessors.emplace(edge);
    }
  }
}

} // namespace call_graph
