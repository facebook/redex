/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CallGraph.h"

#include <utility>

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

namespace mog = method_override_graph;

namespace call_graph {

Graph single_callee_graph(const mog::Graph& method_override_graph,
                          const Scope& scope) {
  return Graph(SingleCalleeStrategy(method_override_graph, scope));
}

Graph complete_call_graph(const mog::Graph& method_override_graph,
                          const Scope& scope) {
  return Graph(CompleteCallGraphStrategy(method_override_graph, scope));
}

Graph multiple_callee_graph(const mog::Graph& method_override_graph,
                            const Scope& scope,
                            uint32_t big_override_threshold) {
  return Graph(MultipleCalleeStrategy(method_override_graph, scope,
                                      big_override_threshold));
}

SingleCalleeStrategy::SingleCalleeStrategy(
    const mog::Graph& method_override_graph, const Scope& scope)
    : m_scope(scope) {
  auto non_virtual_vec =
      mog::get_non_true_virtuals(method_override_graph, scope);
  m_non_virtual.insert(non_virtual_vec.begin(), non_virtual_vec.end());
}

CallSites SingleCalleeStrategy::get_callsites(const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  editable_cfg_adapter::iterate_with_iterator(
      code, [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee = this->resolve_callee(method, insn);
          if (callee == nullptr || is_definitely_virtual(callee)) {
            return editable_cfg_adapter::LOOP_CONTINUE;
            ;
          }
          if (callee->is_concrete()) {
            callsites.emplace_back(callee, insn);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
        ;
      });
  return callsites;
}

RootAndDynamic SingleCalleeStrategy::get_roots() const {
  RootAndDynamic root_and_dynamic;
  auto& roots = root_and_dynamic.roots;
  walk::code(m_scope, [&](DexMethod* method, IRCode& /* code */) {
    if (is_definitely_virtual(method) || root(method) ||
        method::is_clinit(method) || method::is_argless_init(method)) {
      roots.emplace_back(method);
    }
  });
  return root_and_dynamic;
}

bool SingleCalleeStrategy::is_definitely_virtual(DexMethod* method) const {
  return method->is_virtual() && m_non_virtual.count(method) == 0;
}

DexMethod* SingleCalleeStrategy::resolve_callee(const DexMethod* caller,
                                                IRInstruction* invoke) const {
  return resolve_method(invoke->get_method(), opcode_to_search(invoke), caller);
}

MultipleCalleeBaseStrategy::MultipleCalleeBaseStrategy(
    const mog::Graph& method_override_graph, const Scope& scope)
    : SingleCalleeStrategy(method_override_graph, scope),
      m_method_override_graph(method_override_graph) {}

const std::vector<const DexMethod*>&
MultipleCalleeBaseStrategy::get_ordered_overriding_methods_with_code(
    const DexMethod* method) const {
  auto res = m_overriding_methods_cache.get(method, nullptr);
  if (!res) {
    auto overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    std20::erase_if(overriding_methods, [](auto* m) { return !m->get_code(); });
    std::sort(overriding_methods.begin(), overriding_methods.end(),
              compare_dexmethods);
    m_overriding_methods_cache.update(
        method, [&overriding_methods, &res](auto, auto& p, bool exists) {
          if (exists) {
            always_assert(*p == overriding_methods);
          } else {
            p = std::make_shared<std::vector<const DexMethod*>>(
                std::move(overriding_methods));
          }
          res = p;
        });
  }
  return *res;
}

RootAndDynamic MultipleCalleeBaseStrategy::get_roots() const {
  RootAndDynamic root_and_dynamic;
  MethodSet emplaced_methods;
  auto& roots = root_and_dynamic.roots;
  auto& dynamic_methods = root_and_dynamic.dynamic_methods;
  // Gather clinits and root methods, and the methods that override or
  // overriden by the root methods.
  auto add_root_method_overrides = [&](const DexMethod* method) {
    if (!method->get_code() || root(method) || method->is_external()) {
      // No need to add root methods, they will be added anyway.
      return;
    }
    if (emplaced_methods.emplace(method).second) {
      roots.emplace_back(method);
    }
  };
  walk::methods(m_scope, [&](DexMethod* method) {
    if (method::is_clinit(method)) {
      roots.emplace_back(method);
      emplaced_methods.emplace(method);
      return;
    }
    // For methods marked with DoNotInline, we also add to dynamic methods set
    // to avoid propagating return value.
    if (method->rstate.dont_inline()) {
      dynamic_methods.emplace(method);
    }
    if (!root(method) && !method::is_argless_init(method) &&
        !(method->is_virtual() &&
          is_interface(type_class(method->get_class())) &&
          !can_rename(method))) {
      // For root methods and dynamically added classes, created via
      // Proxy.newProxyInstance, we need to add them and their overrides and
      // overriden to roots.
      return;
    }
    if (method->is_virtual() && is_interface(type_class(method->get_class())) &&
        !can_rename(method)) {
      dynamic_methods.emplace(method);
    }
    if (method->get_code() && emplaced_methods.emplace(method).second) {
      roots.emplace_back(method);
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    for (auto overriding_method : overriding_methods) {
      add_root_method_overrides(overriding_method);
    }
    const auto& overiden_methods =
        mog::get_overridden_methods(m_method_override_graph, method);
    for (auto overiden_method : overiden_methods) {
      add_root_method_overrides(overiden_method);
    }
  });
  // Gather methods that override or implement external or native methods
  // as well.
  for (auto& pair : m_method_override_graph.nodes()) {
    auto method = pair.first;
    if (method->is_external()) {
      dynamic_methods.emplace(method);
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, method);
      for (auto* overriding : overriding_methods) {
        if (overriding->is_external()) {
          dynamic_methods.emplace(overriding);
        }
        if (!overriding->is_external() && overriding->get_code() &&
            emplaced_methods.emplace(overriding).second) {
          roots.emplace_back(overriding);
        }
      }
      // Internal methods might be overriden by external methods. Add such
      // methods to dynamic methods to avoid return value propagation as well.
      const auto& overiden_methods =
          mog::get_overridden_methods(m_method_override_graph, method, true);
      for (auto m : overiden_methods) {
        dynamic_methods.emplace(m);
      }
    }
    if (is_native(method)) {
      dynamic_methods.emplace(method);
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, method, true);
      for (auto m : overriding_methods) {
        dynamic_methods.emplace(m);
      }
      const auto& overiden_methods =
          mog::get_overridden_methods(m_method_override_graph, method, true);
      for (auto m : overiden_methods) {
        dynamic_methods.emplace(m);
      }
    }
  }
  // Add additional roots if needed.
  auto additional_roots = get_additional_roots(emplaced_methods);
  roots.insert(roots.end(), additional_roots.begin(), additional_roots.end());
  return root_and_dynamic;
}

CompleteCallGraphStrategy::CompleteCallGraphStrategy(
    const mog::Graph& method_override_graph, const Scope& scope)
    : MultipleCalleeBaseStrategy(method_override_graph, scope) {}

static DexMethod* resolve_interface_virtual_callee(const IRInstruction* insn,
                                                   const DexMethod* caller) {
  DexMethod* callee = nullptr;
  if (opcode_to_search(insn) == MethodSearch::Virtual) {
    callee = resolve_method(insn->get_method(), MethodSearch::InterfaceVirtual,
                            caller);
    if (callee == nullptr) {
      auto insn_method_cls = type_class(insn->get_method()->get_class());
      if (insn_method_cls != nullptr && !insn_method_cls->is_external()) {
        TRACE(CALLGRAPH, 1, "Unexpected unresolved insn %s in %s", SHOW(insn),
              SHOW(caller));
      }
    }
  }
  return callee;
}

CallSites CompleteCallGraphStrategy::get_callsites(
    const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  editable_cfg_adapter::iterate_with_iterator(
      code, [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee = this->resolve_callee(method, insn);
          if (callee == nullptr) {
            callee = resolve_interface_virtual_callee(insn, method);
            if (callee == nullptr) {
              return editable_cfg_adapter::LOOP_CONTINUE;
            }
          }
          if (callee->is_concrete()) {
            callsites.emplace_back(callee, insn);
          }
          if (opcode::is_invoke_virtual(insn->opcode()) ||
              opcode::is_invoke_interface(insn->opcode())) {
            const auto& overriding_methods =
                get_ordered_overriding_methods_with_code(callee);

            for (auto m : overriding_methods) {
              callsites.emplace_back(m, insn);
            }
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
        ;
      });
  return callsites;
}

RootAndDynamic CompleteCallGraphStrategy::get_roots() const {
  RootAndDynamic root_and_dynamic;
  MethodSet emplaced_methods;
  auto& roots = root_and_dynamic.roots;
  auto add_root_method_overrides = [&](const DexMethod* method) {
    if (!root(method) && emplaced_methods.emplace(method).second) {
      // No need to add root methods, they will be added anyway.
      roots.emplace_back(method);
    }
  };
  walk::methods(m_scope, [&](DexMethod* method) {
    if (root(method) || method::is_clinit(method) ||
        method::is_argless_init(method)) {
      if (emplaced_methods.emplace(method).second) {
        roots.emplace_back(method);
      }
    }
    if (!root(method) && !(method->is_virtual() &&
                           is_interface(type_class(method->get_class())) &&
                           !can_rename(method))) {
      // For root methods and dynamically added classes, created via
      // Proxy.newProxyInstance, we need to add them and their overrides and
      // overriden to roots.
      return;
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    for (auto overriding_method : overriding_methods) {
      add_root_method_overrides(overriding_method);
    }
    const auto& overiden_methods =
        mog::get_overridden_methods(m_method_override_graph, method);
    for (auto overiden_method : overiden_methods) {
      add_root_method_overrides(overiden_method);
    }
  });
  // Gather methods that override or implement external methods
  for (auto& pair : m_method_override_graph.nodes()) {
    auto method = pair.first;
    if (method->is_external()) {
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, method, true);
      for (auto* overriding : overriding_methods) {
        if (emplaced_methods.emplace(overriding).second) {
          roots.emplace_back(overriding);
        }
      }
    }
  }
  return root_and_dynamic;
}

MultipleCalleeStrategy::MultipleCalleeStrategy(
    const mog::Graph& method_override_graph,
    const Scope& scope,
    uint32_t big_override_threshold)
    : MultipleCalleeBaseStrategy(method_override_graph, scope) {
  // Gather big overrides true virtual methods.
  ConcurrentSet<const DexMethod*> concurrent_callees;
  ConcurrentSet<const DexMethod*> concurrent_big_overrides;
  walk::parallel::opcodes(scope, [&](const DexMethod* method,
                                     IRInstruction* insn) {
    if (opcode::is_an_invoke(insn->opcode())) {
      auto callee =
          resolve_method(insn->get_method(), opcode_to_search(insn), method);
      if (callee == nullptr) {
        callee = resolve_interface_virtual_callee(insn, method);
        if (callee == nullptr) {
          return;
        }
      }
      if (!callee->is_virtual()) {
        return;
      }
      if (!concurrent_callees.insert(callee)) {
        return;
      }
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, callee);
      uint32_t num_override = 0;
      for (auto overriding_method : overriding_methods) {
        if (overriding_method->get_code()) {
          ++num_override;
        }
      }
      if (num_override > big_override_threshold) {
        concurrent_big_overrides.emplace(callee);
        for (auto overriding_method : overriding_methods) {
          concurrent_big_overrides.emplace(overriding_method);
        }
      }
    }
  });
  m_big_override = concurrent_big_overrides.move_to_container();
}

CallSites MultipleCalleeStrategy::get_callsites(const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  editable_cfg_adapter::iterate_with_iterator(
      code, [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee = this->resolve_callee(method, insn);
          if (callee == nullptr) {
            callee = resolve_interface_virtual_callee(insn, method);
            if (callee == nullptr) {
              return editable_cfg_adapter::LOOP_CONTINUE;
              ;
            }
          }
          if (is_definitely_virtual(callee)) {
            // For true virtual callees, add the callee itself and all of its
            // overrides if they are not in big overrides.
            if (m_big_override.count(callee)) {
              return editable_cfg_adapter::LOOP_CONTINUE;
            }
            if (callee->get_code()) {
              callsites.emplace_back(callee, insn);
            }
            if (insn->opcode() != OPCODE_INVOKE_SUPER) {
              const auto& overriding_methods =
                  get_ordered_overriding_methods_with_code(callee);
              for (auto overriding_method : overriding_methods) {
                callsites.emplace_back(overriding_method, insn);
              }
            }
          } else if (callee->is_concrete()) {
            callsites.emplace_back(callee, insn);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
        ;
      });
  return callsites;
}

// Add big override methods to root as well.
std::vector<const DexMethod*> MultipleCalleeStrategy::get_additional_roots(
    const MethodSet& existing_roots) const {
  std::vector<const DexMethod*> additional_roots;
  for (auto method : m_big_override) {
    if (!method->is_external() && !existing_roots.count(method) &&
        method->get_code()) {
      additional_roots.emplace_back(method);
    }
  }
  return additional_roots;
}

Edge::Edge(NodeId caller, NodeId callee, IRInstruction* invoke_insn)
    : m_caller(caller), m_callee(callee), m_invoke_insn(invoke_insn) {}

Graph::Graph(const BuildStrategy& strat)
    : m_entry(std::make_shared<Node>(Node::GHOST_ENTRY)),
      m_exit(std::make_shared<Node>(Node::GHOST_EXIT)) {
  // Obtain the callsites of each method recursively, building the graph in the
  // process.
  ConcurrentMap<const DexMethod*, NodeId> concurrent_nodes;
  ConcurrentMap<const IRInstruction*, std::unordered_set<const DexMethod*>>
      concurrent_insn_to_callee;
  std::mutex nodes_mutex;
  struct MethodEdges {
    const DexMethod* method;
    std::vector<std::shared_ptr<Edge>> edges;
  };
  ConcurrentMap<NodeId, std::vector<MethodEdges>> concurrent_preds;
  ConcurrentMap<NodeId, std::vector<MethodEdges>> concurrent_succs;
  auto record_trivial_edge = [&](auto caller_node, auto callee_node) {
    auto edge = std::make_shared<Edge>(caller_node, callee_node,
                                       /* invoke_insn */ nullptr);
    concurrent_preds.update(callee_node, [&](auto, auto& v, bool) {
      v.emplace_back((MethodEdges){caller_node->method(), {edge}});
    });
    concurrent_succs.update(caller_node, [&](auto, auto& w, bool) {
      w.emplace_back((MethodEdges){callee_node->method(), {std::move(edge)}});
    });
  };
  struct WorkItem {
    const DexMethod* caller;
    NodeId caller_node;
    bool caller_is_root;
  };
  auto wq = workqueue_foreach<WorkItem>(
      [&](sparta::WorkerState<WorkItem>* worker_state,
          const WorkItem& work_item) {
        auto caller = work_item.caller;
        auto caller_node = work_item.caller_node;
        auto caller_is_root = work_item.caller_is_root;

        if (caller_is_root) {
          // Add edges from the single "ghost" entry node to all the "real"
          // entry nodes in the graph.
          record_trivial_edge(this->entry(), caller_node);
        }

        auto callsites = strat.get_callsites(caller);
        if (callsites.empty()) {
          // Add edges from the single "ghost" exit node to all the "real" exit
          // nodes in the graph.
          record_trivial_edge(caller_node, this->exit());
          return;
        }

        // Gather and create all callee nodes, and kick off new concurrent work
        std::unordered_map<const DexMethod*, size_t> callee_indices;
        struct CalleePartition {
          const DexMethod* callee;
          std::vector<IRInstruction*> invoke_insns{};
        };
        std::vector<CalleePartition> callee_partitions;
        std::unordered_map<const IRInstruction*,
                           std::unordered_set<const DexMethod*>>
            insn_to_callee;
        for (const auto& callsite : callsites) {
          auto callee = callsite.callee;
          auto [it, emplaced] =
              callee_indices.emplace(callee, callee_indices.size());
          if (emplaced) {
            callee_partitions.push_back(CalleePartition{callee});
          }
          auto& callee_partition = callee_partitions[it->second];
          callee_partition.invoke_insns.push_back(callsite.invoke_insn);
          insn_to_callee[callsite.invoke_insn].emplace(callee);
        }

        // Record all edges (we actually add them in a deterministic way later)
        std::vector<MethodEdges> w;
        w.reserve(callee_partitions.size());
        for (auto& callee_partition : callee_partitions) {
          auto callee = callee_partition.callee;
          auto& callee_invoke_insns = callee_partition.invoke_insns;
          NodeId callee_node{};
          bool added{false};
          concurrent_nodes.update(callee, [&](auto, auto& n, bool exists) {
            if (!exists) {
              added = true;
              std::lock_guard<std::mutex> lock_guard(nodes_mutex);
              n = this->make_node(callee);
            }
            callee_node = n;
          });
          if (added) {
            worker_state->push_task(
                (WorkItem){callee, callee_node, /* is_root */ false});
          }

          std::vector<std::shared_ptr<Edge>> edges;
          edges.reserve(callee_invoke_insns.size());
          for (auto* invoke_insn : callee_invoke_insns) {
            edges.push_back(
                std::make_shared<Edge>(caller_node, callee_node, invoke_insn));
          }
          concurrent_preds.update(callee_node, [&](auto, auto& v, bool) {
            v.push_back((MethodEdges){caller, edges});
          });
          w.push_back((MethodEdges){callee, std::move(edges)});
        }
        concurrent_succs.emplace(caller_node, std::move(w));

        // Populate concurrent_insn_to_callee
        for (auto&& [invoke_insn, callees] : insn_to_callee) {
          concurrent_insn_to_callee.emplace(invoke_insn, std::move(callees));
        }
      },
      redex_parallel::default_num_threads(),
      /*push_tasks_while_running=*/true);

  auto root_and_dynamic = strat.get_roots();
  const auto& roots = root_and_dynamic.roots;
  m_dynamic_methods = std::move(root_and_dynamic.dynamic_methods);
  for (const DexMethod* root : roots) {
    auto root_node = make_node(root);
    auto emplaced = concurrent_nodes.emplace_unsafe(root, root_node);
    always_assert(emplaced);
    wq.add_item((WorkItem){root, root_node, /* is_root */ true});
  }
  wq.run_all();

  // Fill in all predecessors and successors, and sort them
  auto wq2 = workqueue_foreach<NodeId>([&](NodeId node) {
    auto linearize = [node](auto& m, auto& res) {
      auto it = m.find(node);
      if (it == m.end()) {
        return;
      }
      auto& v = it->second;
      std::sort(v.begin(), v.end(), [](auto& p, auto& q) {
        return compare_dexmethods(p.method, q.method);
      });
      for (auto& me : v) {
        res.insert(res.end(), std::make_move_iterator(me.edges.begin()),
                   std::make_move_iterator(me.edges.end()));
      }
    };
    linearize(concurrent_succs, node->m_successors);
    linearize(concurrent_preds, node->m_predecessors);
  });
  wq2.add_item(m_entry.get());
  wq2.add_item(m_exit.get());
  for (auto&& [_, node] : m_nodes) {
    wq2.add_item(node.get());
  }
  wq2.run_all();

  m_insn_to_callee = concurrent_insn_to_callee.move_to_container();
}

NodeId Graph::make_node(const DexMethod* m) {
  auto [it, inserted] = m_nodes.emplace(m, nullptr);
  if (inserted) {
    it->second = std::make_shared<Node>(m);
  }

  return it->second.get();
}

void Graph::add_edge(const NodeId& caller,
                     const NodeId& callee,
                     IRInstruction* invoke_insn) {
  auto edge = std::make_shared<Edge>(caller, callee, invoke_insn);
  caller->m_successors.emplace_back(edge);
  callee->m_predecessors.emplace_back(std::move(edge));
}

MethodSet resolve_callees_in_graph(const Graph& graph,
                                   const DexMethod* method,
                                   const IRInstruction* insn) {

  always_assert(insn);
  MethodSet ret;
  for (const auto& edge_id : graph.node(method)->callees()) {
    auto invoke_insn = edge_id->invoke_insn();
    if (invoke_insn == insn) {
      auto callee_node_id = edge_id->callee();
      if (callee_node_id) {
        auto callee = callee_node_id->method();
        if (callee) {
          ret.emplace(callee);
        }
      }
    }
  }
  return ret;
}

const MethodSet& resolve_callees_in_graph(const Graph& graph,
                                          const IRInstruction* insn) {
  const auto& insn_to_callee = graph.get_insn_to_callee();
  auto it = insn_to_callee.find(insn);
  if (it != insn_to_callee.end()) {
    return it->second;
  }
  static const MethodSet no_methods;
  return no_methods;
}

bool method_is_dynamic(const Graph& graph, const DexMethod* method) {
  return graph.get_dynamic_methods().count(method);
}

CallgraphStats get_num_nodes_edges(const Graph& graph) {
  std::unordered_set<NodeId> visited_node;
  std::queue<NodeId> to_visit;
  uint32_t num_edge = 0;
  uint32_t num_callsites = 0;
  to_visit.push(graph.entry());
  while (!to_visit.empty()) {
    auto front = to_visit.front();
    to_visit.pop();
    if (visited_node.emplace(front).second) {
      num_edge += front->callees().size();
      std::unordered_set<IRInstruction*> callsites;
      for (const auto& edge : front->callees()) {
        to_visit.push(edge->callee());
        auto invoke_insn = edge->invoke_insn();
        if (invoke_insn) {
          callsites.emplace(invoke_insn);
        }
      }
      num_callsites += callsites.size();
    }
  }
  return CallgraphStats(visited_node.size(), num_edge, num_callsites);
}

} // namespace call_graph
