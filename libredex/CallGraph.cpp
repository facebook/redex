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

Edge::Edge(DexMethod* caller, DexMethod* callee, FatMethod::iterator invoke_it)
    : m_caller(caller), m_callee(callee), m_invoke_it(invoke_it) {}

Graph::Graph(const Scope& scope, bool include_virtuals) {
  MethodRefCache resolved_refs;
  auto resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, resolved_refs);
  };
  auto non_virtual_vec =
      include_virtuals ? devirtualize(scope) : std::vector<DexMethod*>();
  auto non_virtual = std::unordered_set<const DexMethod*>(
      non_virtual_vec.begin(), non_virtual_vec.end());
  auto is_definitely_virtual = [&](const DexMethod* method) {
    return method->is_virtual() && non_virtual.count(method) == 0;
  };
  walk::methods(scope, [&](DexMethod* caller) {
    auto* code = caller->get_code();
    if (!code) {
      return;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        auto callee = resolver(insn->get_method(), opcode_to_search(insn));
        if (callee == nullptr || is_definitely_virtual(callee)) {
          continue;
        }
        if (callee->is_concrete()) {
          add_edge(caller, callee, code->iterator_to(mie));
        }
      }
    }
  });

  // Add edges from the single "ghost" entry node to all the 'real' entry
  // nodes in the graph. We consider a node to be a potential entry point if
  // it is virtual or if it is marked by a Proguard keep rule.
  for (auto& pair : m_nodes) {
    auto method = pair.first;
    if (is_definitely_virtual(method) || root(method)) {
      auto edge = std::make_shared<Edge>(
          nullptr, const_cast<DexMethod*>(method), FatMethod::iterator());
      m_entry.m_successors.emplace_back(edge);
      pair.second.m_predecessors.emplace_back(edge);
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
                     FatMethod::iterator invoke_it) {
  auto edge = std::make_shared<Edge>(caller, callee, invoke_it);
  make_node(caller).m_successors.emplace_back(edge);
  make_node(callee).m_predecessors.emplace_back(edge);
}

} // namespace call_graph
