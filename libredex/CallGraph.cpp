/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CallGraph.h"

#include "MethodOverrideGraph.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

using namespace call_graph;

class SingleCalleeStrategy final : public BuildStrategy {
 public:
  SingleCalleeStrategy(const Scope& scope) : m_scope(scope) {
    auto non_virtual_vec = mog::get_non_true_virtuals(scope);
    m_non_virtual.insert(non_virtual_vec.begin(), non_virtual_vec.end());
  }

  CallSites get_callsites(const DexMethod* method) const override {
    CallSites callsites;
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return callsites;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        auto callee = resolve_method(
            insn->get_method(), opcode_to_search(insn), m_resolved_refs);
        if (callee == nullptr || is_definitely_virtual(callee)) {
          continue;
        }
        if (callee->is_concrete()) {
          callsites.emplace_back(callee, code->iterator_to(mie));
        }
      }
    }
    return callsites;
  }

  std::vector<const DexMethod*> get_roots() const override {
    std::vector<const DexMethod*> roots;

    walk::code(m_scope, [&](DexMethod* method, IRCode& code) {
      if (is_definitely_virtual(method) || root(method) ||
          method::is_clinit(method)) {
        roots.emplace_back(method);
      }
    });
    return roots;
  }

 private:
  bool is_definitely_virtual(DexMethod* method) const {
    return method->is_virtual() && m_non_virtual.count(method) == 0;
  }

  const Scope& m_scope;
  std::unordered_set<DexMethod*> m_non_virtual;
  mutable MethodRefCache m_resolved_refs;
};

} // namespace

namespace call_graph {

Graph single_callee_graph(const Scope& scope) {
  return Graph(SingleCalleeStrategy(scope));
}

Edge::Edge(const DexMethod* caller,
           const DexMethod* callee,
           const IRList::iterator& invoke_it)
    : m_caller(caller), m_callee(callee), m_invoke_it(invoke_it) {}

Graph::Graph(const BuildStrategy& strat) {
  // Add edges from the single "ghost" entry node to all the "real" entry
  // nodes in the graph.
  auto roots = strat.get_roots();
  for (const DexMethod* root : roots) {
    auto edge = std::make_shared<Edge>(nullptr, root, IRList::iterator());
    m_entry.m_successors.emplace_back(edge);
    make_node(root).m_predecessors.emplace_back(edge);
  }

  // Obtain the callsites of each method recursively, building the graph in the
  // process.
  std::unordered_set<const DexMethod*> visited;
  auto visit = [&](const auto* caller) {
    auto visit_impl = [&](const auto* caller, auto& visit_fn) {
      if (visited.count(caller) != 0) {
        return;
      }
      visited.emplace(caller);
      for (const auto& callsite : strat.get_callsites(caller)) {
        this->add_edge(caller, callsite.callee, callsite.invoke);
        visit_fn(callsite.callee, visit_fn);
      }
    };
    visit_impl(caller, visit_impl);
  };

  for (const DexMethod* root : roots) {
    visit(root);
  }
}

Node& Graph::make_node(const DexMethod* m) {
  auto it = m_nodes.find(m);
  if (it != m_nodes.end()) {
    return it->second;
  }
  m_nodes.emplace(m, Node(m));
  return m_nodes.at(m);
}

void Graph::add_edge(const DexMethod* caller,
                     const DexMethod* callee,
                     const IRList::iterator& invoke_it) {
  auto edge = std::make_shared<Edge>(caller, callee, invoke_it);
  make_node(caller).m_successors.emplace_back(edge);
  make_node(callee).m_predecessors.emplace_back(edge);
}

} // namespace call_graph
