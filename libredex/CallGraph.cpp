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

namespace {

AccumulatingTimer s_timer("CallGraph");

} // namespace

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
    : m_scope(scope),
      m_non_virtual(mog::get_non_true_virtuals(method_override_graph, scope)) {}

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
          auto callee = resolve_invoke_method(insn, method);
          if (callee == nullptr || is_definitely_virtual(callee)) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          if (callee->is_concrete()) {
            callsites.emplace_back(callee, insn);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return callsites;
}

RootAndDynamic SingleCalleeStrategy::get_roots() const {
  RootAndDynamic root_and_dynamic;
  auto& roots = root_and_dynamic.roots;
  walk::code(m_scope, [&](DexMethod* method, IRCode& /* code */) {
    if (is_definitely_virtual(method) || root(method) ||
        method::is_clinit(method) || method::is_argless_init(method)) {
      roots.insert(method);
    }
  });
  return root_and_dynamic;
}

bool SingleCalleeStrategy::is_definitely_virtual(DexMethod* method) const {
  return method->is_virtual() && m_non_virtual.count_unsafe(method) == 0;
}

MultipleCalleeBaseStrategy::MultipleCalleeBaseStrategy(
    const mog::Graph& method_override_graph, const Scope& scope)
    : SingleCalleeStrategy(method_override_graph, scope),
      m_method_override_graph(method_override_graph) {}

const std::vector<const DexMethod*>&
MultipleCalleeBaseStrategy::get_ordered_overriding_methods_with_code_or_native(
    const DexMethod* method) const {
  auto res = m_overriding_methods_cache.get(method);
  if (res) {
    return *res;
  }
  return init_ordered_overriding_methods_with_code_or_native(
      method, mog::get_overriding_methods(m_method_override_graph, method));
}

const std::vector<const DexMethod*>&
MultipleCalleeBaseStrategy::init_ordered_overriding_methods_with_code_or_native(
    const DexMethod* method,
    std::vector<const DexMethod*> overriding_methods) const {
  std20::erase_if(overriding_methods,
                  [](auto* m) { return !m->get_code() && !is_native(m); });
  std::sort(overriding_methods.begin(), overriding_methods.end(),
            compare_dexmethods);
  return *m_overriding_methods_cache
              .get_or_emplace_and_assert_equal(method,
                                               std::move(overriding_methods))
              .first;
}

RootAndDynamic MultipleCalleeBaseStrategy::get_roots() const {
  Timer t("get_roots");
  RootAndDynamic root_and_dynamic;
  auto& roots = root_and_dynamic.roots;
  auto& dynamic_methods = root_and_dynamic.dynamic_methods;
  // Gather clinits and root methods, and the methods that override or
  // overriden by the root methods.
  auto add_root_method_overrides = [&](const DexMethod* method) {
    if (!method->get_code() || root(method) || method->is_external()) {
      // No need to add root methods, they will be added anyway.
      return;
    }
    roots.insert(method);
  };
  walk::methods(m_scope, [&](DexMethod* method) {
    if (method::is_clinit(method)) {
      roots.insert(method);
      return;
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
    if (method->get_code()) {
      roots.insert(method);
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(m_method_override_graph, method);
    for (auto overriding_method : overriding_methods) {
      add_root_method_overrides(overriding_method);
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
        // We don't need to add overriding to dynamic_methods here, as that will
        // happen anyway.
        if (!overriding->is_external() && overriding->get_code()) {
          roots.insert(overriding);
        }
      }
      // We don't need to add overridden external methods to dynamic_methods, as
      // that will happen anyway. Internal interface methods can be overridden
      // by external methods as well.
      const auto& overridden_methods =
          mog::get_overridden_methods(m_method_override_graph, method, true);
      for (auto m : overridden_methods) {
        if (!m->is_external()) {
          auto* cls = type_class(m->get_class());
          dynamic_methods.emplace(m);
        }
      }
    }
  }
  return root_and_dynamic;
}

CompleteCallGraphStrategy::CompleteCallGraphStrategy(
    const mog::Graph& method_override_graph, const Scope& scope)
    : MultipleCalleeBaseStrategy(method_override_graph, scope) {}

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
          auto callee = resolve_invoke_method(insn, method);
          if (callee == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          if (callee->get_code() || is_native(callee)) {
            callsites.emplace_back(callee, insn);
          }
          if (opcode::is_invoke_virtual(insn->opcode()) ||
              opcode::is_invoke_interface(insn->opcode())) {
            const auto& overriding_methods =
                get_ordered_overriding_methods_with_code_or_native(callee);
            for (auto overriding_method : overriding_methods) {
              callsites.emplace_back(overriding_method, insn);
            }
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return callsites;
}

RootAndDynamic CompleteCallGraphStrategy::get_roots() const {
  RootAndDynamic root_and_dynamic;
  auto& roots = root_and_dynamic.roots;
  auto add_root_method_overrides = [&](const DexMethod* method) {
    if (!root(method)) {
      // No need to add root methods, they will be added anyway.
      roots.insert(method);
    }
  };
  walk::methods(m_scope, [&](DexMethod* method) {
    if (root(method) || method::is_clinit(method) ||
        method::is_argless_init(method)) {
      roots.insert(method);
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
  });
  // Gather methods that override or implement external methods
  for (auto& pair : m_method_override_graph.nodes()) {
    auto method = pair.first;
    if (method->is_external()) {
      const auto& overriding_methods =
          mog::get_overriding_methods(m_method_override_graph, method, true);
      for (auto* overriding : overriding_methods) {
        roots.insert(overriding);
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
  walk::parallel::opcodes(
      scope, [&](const DexMethod* method, IRInstruction* insn) {
        if (opcode::is_an_invoke(insn->opcode())) {
          auto callee = resolve_invoke_method(insn, method);
          if (callee == nullptr) {
            return;
          }
          if (!callee->is_virtual() || insn->opcode() == OPCODE_INVOKE_SUPER) {
            return;
          }
          if (!concurrent_callees.insert(callee)) {
            return;
          }
          auto overriding_methods =
              mog::get_overriding_methods(m_method_override_graph, callee);
          uint32_t num_override = 0;
          for (auto overriding_method : overriding_methods) {
            if (overriding_method->get_code()) {
              ++num_override;
            }
          }
          if (num_override <= big_override_threshold) {
            init_ordered_overriding_methods_with_code_or_native(
                callee, std::move(overriding_methods));
          } else {
            m_big_virtuals.emplace(callee);
            m_big_virtual_overrides.insert(overriding_methods.begin(),
                                           overriding_methods.end());
          }
        }
      });
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
          auto callee = resolve_invoke_method(insn, method);
          if (callee == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          if (is_definitely_virtual(callee) &&
              insn->opcode() != OPCODE_INVOKE_SUPER) {
            // For true virtual callees, add the callee itself and all of its
            // overrides if they are not in big virtuals.
            if (m_big_virtuals.count_unsafe(callee)) {
              return editable_cfg_adapter::LOOP_CONTINUE;
            }
            if (callee->get_code() || is_native(callee)) {
              callsites.emplace_back(callee, insn);
            }
            const auto& overriding_methods =
                get_ordered_overriding_methods_with_code_or_native(callee);
            for (auto overriding_method : overriding_methods) {
              callsites.emplace_back(overriding_method, insn);
            }
          } else if (callee->is_concrete()) {
            callsites.emplace_back(callee, insn);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return callsites;
}

// Add big override methods to root as well.
RootAndDynamic MultipleCalleeStrategy::get_roots() const {
  auto root_and_dynamic = MultipleCalleeBaseStrategy::get_roots();
  auto add_root = [&](auto* method) {
    if (!method->is_external() && method->get_code()) {
      root_and_dynamic.roots.insert(method);
    }
  };
  std::for_each(m_big_virtuals.begin(), m_big_virtuals.end(), add_root);
  std::for_each(m_big_virtual_overrides.begin(), m_big_virtual_overrides.end(),
                add_root);
  return root_and_dynamic;
}

Edge::Edge(NodeId caller, NodeId callee, IRInstruction* invoke_insn)
    : m_caller(caller), m_callee(callee), m_invoke_insn(invoke_insn) {}

Graph::Graph(const BuildStrategy& strat)
    : m_entry(std::make_unique<Node>(Node::GHOST_ENTRY)),
      m_exit(std::make_unique<Node>(Node::GHOST_EXIT)) {
  auto timer_scope = s_timer.scope();
  Timer t("Graph::Graph");

  auto root_and_dynamic = strat.get_roots();
  m_dynamic_methods = std::move(root_and_dynamic.dynamic_methods);
  std::vector<Node*> root_nodes;

  // Obtain the callsites of each method recursively, building the graph in the
  // process.
  ConcurrentMap<NodeId, std::list<std::vector<const Edge*>>> concurrent_preds;
  std::mutex predecessors_wq_mutex;
  auto predecessors_wq = workqueue_foreach<Node*>([&](Node* callee_node) {
    auto& preds = concurrent_preds.at_unsafe(callee_node);
    std::vector<std::vector<const Edge*>> callee_edges;
    callee_edges.reserve(preds.size());
    size_t size = 0;
    for (auto& edges : preds) {
      size += edges.size();
      callee_edges.emplace_back(std::move(edges));
    }
    std::sort(callee_edges.begin(), callee_edges.end(), [](auto& p, auto& q) {
      return compare_dexmethods(p.front()->caller()->method(),
                                q.front()->caller()->method());
    });
    auto& callee_predecessors = callee_node->m_predecessors;
    callee_predecessors.reserve(size);
    for (auto& edges : callee_edges) {
      callee_predecessors.insert(callee_predecessors.end(), edges.begin(),
                                 edges.end());
    }
  });

  struct WorkItem {
    const DexMethod* caller;
    Node* caller_node;
  };
  constexpr IRInstruction* no_insn = nullptr;
  auto successors_wq = workqueue_foreach<WorkItem>(
      [&](sparta::WorkerState<WorkItem>* worker_state,
          const WorkItem& work_item) {
        auto get_node = [&](const DexMethod* method) -> Node* {
          auto [const_node, node_created] =
              m_nodes.get_or_emplace_and_assert_equal(method, method);
          Node* node = const_cast<Node*>(const_node);
          if (node_created) {
            worker_state->push_task((WorkItem){method, node});
          }
          return node;
        };

        using Insns = std::vector<IRInstruction*>;
        struct CalleePartition {
          Node* callee_node;
          Insns invoke_insns;
          CalleePartition(Node* callee_node, Insns invoke_insns)
              : callee_node(callee_node),
                invoke_insns(std::move(invoke_insns)) {}
        };
        std::vector<CalleePartition> callee_partitions;
        std::unordered_map<const IRInstruction*,
                           std::unordered_set<const DexMethod*>>
            insn_to_callee;
        size_t caller_successors_size;

        auto* caller = work_item.caller;
        if (caller == nullptr) {
          // Add edges from the single "ghost" entry node to all the "real" root
          // entry nodes in the graph.
          callee_partitions.reserve(root_nodes.size());
          for (auto root_node : root_nodes) {
            callee_partitions.emplace_back(root_node, Insns{no_insn});
          }
          caller_successors_size = root_nodes.size();
        } else {
          auto callsites = strat.get_callsites(caller);
          if (callsites.empty()) {
            // Add edges from the single "ghost" exit node to all the "real"
            // exit nodes in the graph.
            callee_partitions.emplace_back(m_exit.get(), Insns{no_insn});
            caller_successors_size = 1;
          } else {
            // Gather and create all "real" callee nodes, and kick off new
            // concurrent work
            std::unordered_map<const DexMethod*, size_t> callee_indices;
            for (const auto& callsite : callsites) {
              auto callee = callsite.callee;
              auto [it, emplaced] =
                  callee_indices.emplace(callee, callee_indices.size());
              if (emplaced) {
                callee_partitions.emplace_back(get_node(callee), Insns());
              }
              auto& callee_partition = callee_partitions[it->second];
              callee_partition.invoke_insns.push_back(callsite.invoke_insn);
              insn_to_callee[callsite.invoke_insn].emplace(callee);
            }
            caller_successors_size = callsites.size();
          }
        }

        // Record all edges
        auto* caller_node = work_item.caller_node;
        auto& caller_successors = caller_node->m_successors;
        caller_successors.reserve(caller_successors_size);
        std::sort(callee_partitions.begin(), callee_partitions.end(),
                  [](auto& p, auto& q) {
                    return compare_dexmethods(p.callee_node->method(),
                                              q.callee_node->method());
                  });
        std::vector<Node*> added_preds;
        for (auto&& [callee_node, callee_invoke_insns] : callee_partitions) {
          std::vector<const Edge*> callee_edges;
          callee_edges.reserve(callee_invoke_insns.size());
          for (auto* invoke_insn : callee_invoke_insns) {
            caller_successors.emplace_back(caller_node, callee_node,
                                           invoke_insn);
            callee_edges.push_back(&caller_successors.back());
          }
          bool preds_added;
          concurrent_preds.update(callee_node,
                                  [&](auto, auto& preds, bool exists) {
                                    preds_added = !exists;
                                    preds.emplace_back(std::move(callee_edges));
                                  });
          if (preds_added) {
            added_preds.push_back(callee_node);
          }
        }

        // Schedule postprocessing of predecessors of newly-added preds.
        if (!added_preds.empty()) {
          std::lock_guard<std::mutex> lock_guard(predecessors_wq_mutex);
          for (auto* node : added_preds) {
            predecessors_wq.add_item(node);
          }
        }

        // Populate insn-to-callee map
        for (auto&& [invoke_insn, callees] : insn_to_callee) {
          m_insn_to_callee.emplace(invoke_insn, std::move(callees));
        }
      },
      redex_parallel::default_num_threads(),
      /*push_tasks_while_running=*/true);

  successors_wq.add_item((WorkItem){/* caller */ nullptr, m_entry.get()});
  root_nodes.reserve(root_and_dynamic.roots.size());
  for (const DexMethod* root : root_and_dynamic.roots) {
    auto [root_node, emplaced] = m_nodes.emplace_unsafe(root, root);
    always_assert(emplaced);
    successors_wq.add_item((WorkItem){root, root_node});
    root_nodes.emplace_back(root_node);
  }
  successors_wq.run_all();
  predecessors_wq.run_all();
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

bool invoke_is_dynamic(const Graph& graph, const IRInstruction* insn) {
  auto* callee = resolve_invoke_method(insn);
  if (callee == nullptr) {
    return true;
  }
  // For methods marked with DoNotInline, we also treat them like dynamic
  // methods to avoid propagating return value.
  if (callee->rstate.dont_inline()) {
    return true;
  }
  if (insn->opcode() != OPCODE_INVOKE_VIRTUAL &&
      insn->opcode() != OPCODE_INVOKE_INTERFACE) {
    return false;
  }
  return graph.get_dynamic_methods().count(callee);
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

const MethodVector& Graph::get_callers(const DexMethod* callee) const {
  return *m_callee_to_callers
              .get_or_create_and_assert_equal(
                  callee,
                  [&](const DexMethod*) {
                    std::unordered_set<const DexMethod*> set;
                    if (has_node(callee)) {
                      for (const auto& edge : node(callee)->callers()) {
                        set.insert(edge->caller()->method());
                      }
                      set.erase(nullptr);
                    }
                    return MethodVector(set.begin(), set.end());
                  })
              .first;
}

} // namespace call_graph
