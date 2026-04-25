/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StaticFieldDependencyGraph.h"

#include <chrono>

#include "EditableCfgAdapter.h"
#include "IRCode.h"
#include "IRInstruction.h"

#include "MethodOverrideGraph.h"
#include "MethodUtil.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"

namespace clinit_batching {

void StaticFieldDependencyGraph::walk_clinit_chain(
    DexClass* cls,
    const UnorderedSet<DexClass*>& candidate_classes,
    UnorderedSet<const DexMethod*>& visited,
    UnorderedSet<DexClass*>& found_deps,
    const method_override_graph::Graph* override_graph,
    bool skip_benign) const {
  while (cls != nullptr && !cls->is_external()) {
    if (candidate_classes.count(cls) != 0) {
      found_deps.insert(cls);
    }
    DexMethod* clinit = cls->get_clinit();
    if (clinit != nullptr) {
      find_transitive_dependencies(clinit, candidate_classes, visited,
                                   found_deps, override_graph, skip_benign);
    }
    auto* super_type = cls->get_super_class();
    cls = super_type != nullptr ? type_class(super_type) : nullptr;
  }
}

void StaticFieldDependencyGraph::find_transitive_dependencies(
    const DexMethod* method,
    const UnorderedSet<DexClass*>& candidate_classes,
    UnorderedSet<const DexMethod*>& visited,
    UnorderedSet<DexClass*>& found_deps,
    const method_override_graph::Graph* override_graph,
    bool skip_benign) const {
  if (method == nullptr || method->get_code() == nullptr) {
    return;
  }

  // Avoid infinite recursion
  if (visited.count(method) != 0) {
    return;
  }
  visited.insert(method);

  // InstructionIterable requires non-const access for implementation reasons,
  // but we only read instructions. This const_cast is safe.
  auto* code = const_cast<IRCode*>(method->get_code());
  cfg_adapter::iterate(code, [&](MethodItemEntry& mie) {
    auto* insn = mie.insn;
    auto opcode = insn->opcode();

    // Check for static field access (SGET*/SPUT*)
    // Both reads and writes to static fields trigger class loading.
    if (opcode::is_an_sget(opcode) || opcode::is_an_sput(opcode)) {
      auto* field_ref = insn->get_field();
      always_assert(field_ref != nullptr);
      auto* resolved_field = resolve_field(field_ref, FieldSearch::Static);
      DexClass* dep_class = nullptr;
      if (resolved_field != nullptr) {
        dep_class = type_class(resolved_field->get_class());
      } else {
        dep_class = type_class(field_ref->get_class());
      }
      if (dep_class != nullptr && candidate_classes.count(dep_class) != 0) {
        found_deps.insert(dep_class);
      }
      // Walk the superclass clinit chain for transitive dependencies.
      if (dep_class != nullptr && !dep_class->is_external()) {
        walk_clinit_chain(dep_class, candidate_classes, visited, found_deps,
                          override_graph, skip_benign);
      }
    }
    // Check for static method invocation (INVOKE_STATIC)
    else if (opcode::is_invoke_static(opcode)) {
      auto* method_ref = insn->get_method();
      always_assert(method_ref != nullptr);
      auto* resolved_method = resolve_method(method_ref, MethodSearch::Static);
      DexClass* dep_class = nullptr;
      if (resolved_method != nullptr) {
        dep_class = type_class(resolved_method->get_class());
        // Recursively analyze the called method
        find_transitive_dependencies(resolved_method, candidate_classes,
                                     visited, found_deps, override_graph,
                                     skip_benign);
      } else {
        dep_class = type_class(method_ref->get_class());
      }
      if (dep_class != nullptr && candidate_classes.count(dep_class) != 0) {
        found_deps.insert(dep_class);
      }
      // Walk the superclass clinit chain for transitive dependencies.
      if (dep_class != nullptr && !dep_class->is_external()) {
        walk_clinit_chain(dep_class, candidate_classes, visited, found_deps,
                          override_graph, skip_benign);
      }
    }
    // Check for constructor/direct method calls (INVOKE_DIRECT)
    else if (opcode::is_invoke_direct(opcode)) {
      auto* method_ref = insn->get_method();
      always_assert(method_ref != nullptr);
      auto* resolved_method = resolve_method(method_ref, MethodSearch::Direct);
      if (resolved_method != nullptr) {
        // Recursively analyze the constructor/direct method
        find_transitive_dependencies(resolved_method, candidate_classes,
                                     visited, found_deps, override_graph,
                                     skip_benign);
        // The callee's class clinit chain is also triggered
        auto* callee_class = type_class(resolved_method->get_class());
        if (callee_class != nullptr && !callee_class->is_external()) {
          walk_clinit_chain(callee_class, candidate_classes, visited,
                            found_deps, override_graph, skip_benign);
        }
      }
    }
    // Check for new instance creation (NEW_INSTANCE)
    // new-instance triggers class initialization (clinit). The actual
    // constructor call is a separate invoke-direct instruction, which the
    // invoke-direct handler above will process.
    else if (opcode == OPCODE_NEW_INSTANCE) {
      auto* inst_class = type_class(insn->get_type());
      if (inst_class != nullptr && !inst_class->is_external()) {
        // Walk the superclass clinit chain for transitive dependencies.
        walk_clinit_chain(inst_class, candidate_classes, visited, found_deps,
                          override_graph, skip_benign);
      }
      // Also add the class itself as a dependency if it's a candidate
      if (inst_class != nullptr && candidate_classes.count(inst_class) != 0) {
        found_deps.insert(inst_class);
      }
    }
    // invoke-super: resolve via manual superclass walk from the caller's class.
    // resolve_method(ref, Super) without a caller falls back to a Virtual
    // search from the ref's declaring class, which is incorrect (T132919742).
    else if (opcode::is_invoke_super(opcode)) {
      auto* method_ref = insn->get_method();
      if (skip_benign && method::is_clinit_invoked_method_benign(method_ref)) {
        return cfg_adapter::LOOP_CONTINUE;
      }
      auto* caller_cls = type_class(method->get_class());
      const DexMethod* resolved = nullptr;
      if (caller_cls != nullptr) {
        auto* super_type = caller_cls->get_super_class();
        auto* current = super_type ? type_class(super_type) : nullptr;
        while (current != nullptr && !current->is_external()) {
          for (auto* vm : current->get_vmethods()) {
            if (vm->get_name() == method_ref->get_name() &&
                vm->get_proto() == method_ref->get_proto()) {
              resolved = vm;
              current = nullptr; // break outer loop
              break;
            }
          }
          if (current != nullptr) {
            auto* next_super = current->get_super_class();
            current = next_super ? type_class(next_super) : nullptr;
          }
        }
      }
      if (resolved != nullptr) {
        if (skip_benign && method::is_clinit_invoked_method_benign(resolved)) {
          return cfg_adapter::LOOP_CONTINUE;
        }
        find_transitive_dependencies(resolved, candidate_classes, visited,
                                     found_deps, override_graph, skip_benign);
        auto* callee_class = type_class(resolved->get_class());
        if (callee_class != nullptr && !callee_class->is_external()) {
          walk_clinit_chain(callee_class, candidate_classes, visited,
                            found_deps, override_graph, skip_benign);
        }
      }
    }
    // Check for virtual/interface calls when override graph is available
    else if ((opcode::is_invoke_virtual(opcode) ||
              opcode::is_invoke_interface(opcode)) &&
             override_graph != nullptr) {
      auto* method_ref = insn->get_method();
      // Skip known-benign virtual/interface calls (e.g. HashMap.put,
      // StringBuilder.append). The safety check already skipped these,
      // so the dep graph must do the same to avoid exploring huge
      // call graphs that were never validated.
      if (skip_benign && method::is_clinit_invoked_method_benign(method_ref)) {
        return cfg_adapter::LOOP_CONTINUE;
      }
      auto* resolved = resolve_method(method_ref, opcode_to_search(insn));
      if (resolved != nullptr) {
        if (skip_benign && method::is_clinit_invoked_method_benign(resolved)) {
          return cfg_adapter::LOOP_CONTINUE;
        }
        // Follow the resolved method itself
        find_transitive_dependencies(resolved, candidate_classes, visited,
                                     found_deps, override_graph, skip_benign);
        // Follow all override targets
        auto overriders = method_override_graph::get_overriding_methods(
            *override_graph, resolved, /* include_interfaces */ true);
        for (const auto* overrider : UnorderedIterable(overriders)) {
          find_transitive_dependencies(overrider, candidate_classes, visited,
                                       found_deps, override_graph, skip_benign);
        }
        // Walk the callee's class clinit chain
        auto* callee_class = type_class(resolved->get_class());
        if (callee_class != nullptr && !callee_class->is_external()) {
          walk_clinit_chain(callee_class, candidate_classes, visited,
                            found_deps, override_graph, skip_benign);
        }
      }
    }

    return cfg_adapter::LOOP_CONTINUE;
  });
}

void StaticFieldDependencyGraph::build(
    const UnorderedMap<DexMethod*, DexClass*>& candidate_clinits,
    const method_override_graph::Graph* override_graph,
    bool skip_benign) {
  // First, build a set of all candidate classes for quick lookup
  UnorderedSet<DexClass*> candidate_classes;
  for (const auto& [method, cls] : UnorderedIterable(candidate_clinits)) {
    candidate_classes.insert(cls);
    m_classes.insert(cls);
  }

  TRACE(CLINIT_BATCHING,
        2,
        "StaticFieldDependencyGraph: building graph for %zu classes",
        candidate_classes.size());

  // Analyze each clinit to find dependencies. find_transitive_dependencies
  // already handles all relevant opcodes (SGET/SPUT, INVOKE_STATIC,
  // INVOKE_DIRECT, NEW_INSTANCE, INVOKE_SUPER, INVOKE_VIRTUAL/INTERFACE)
  // and follows call chains transitively, so we just call it on each clinit
  // and record the discovered dependencies as graph edges.
  size_t build_idx = 0;
  for (const auto& [clinit, cls] : UnorderedIterable(candidate_clinits)) {
    build_idx++;
    always_assert(clinit->get_code() != nullptr);
    TRACE(CLINIT_BATCHING, 2, "dep_graph build[%zu/%zu]: starting %s",
          build_idx, candidate_clinits.size(), SHOW(clinit));
    auto build_start = std::chrono::steady_clock::now();

    UnorderedSet<const DexMethod*> visited;
    UnorderedSet<DexClass*> found_deps;
    find_transitive_dependencies(clinit, candidate_classes, visited, found_deps,
                                 override_graph, skip_benign);
    for (auto* dep : UnorderedIterable(found_deps)) {
      if (dep != cls) {
        add_dependency(cls, dep);
        TRACE(CLINIT_BATCHING, 4,
              "StaticFieldDependencyGraph: %s depends on %s", SHOW(cls),
              SHOW(dep));
      }
    }

    auto build_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - build_start)
            .count();
    TRACE(CLINIT_BATCHING, 2, "dep_graph build[%zu/%zu]: %s took %lldms",
          build_idx, candidate_clinits.size(), SHOW(clinit),
          (long long)build_elapsed_ms);
  }

  TRACE(CLINIT_BATCHING,
        2,
        "StaticFieldDependencyGraph: built graph with %zu nodes, %zu edges",
        m_classes.size(),
        [this]() {
          size_t edge_count = 0;
          for (const auto& [cls, deps] : UnorderedIterable(m_dependencies)) {
            edge_count += deps.size();
          }
          return edge_count;
        }());
}

void StaticFieldDependencyGraph::add_dependency(DexClass* from_class,
                                                DexClass* to_class) {
  m_classes.insert(from_class);
  m_classes.insert(to_class);
  m_dependencies[from_class].insert(to_class);
  m_dependents[to_class].insert(from_class);
}

const UnorderedSet<DexClass*>& StaticFieldDependencyGraph::get_dependencies(
    DexClass* cls) const {
  auto it = m_dependencies.find(cls);
  if (it == m_dependencies.end()) {
    return empty_set();
  }
  return it->second;
}

const UnorderedSet<DexClass*>& StaticFieldDependencyGraph::get_dependents(
    DexClass* cls) const {
  auto it = m_dependents.find(cls);
  if (it == m_dependents.end()) {
    return empty_set();
  }
  return it->second;
}

DexClass* StaticFieldDependencyGraph::topological_sort_visit(
    DexClass* cls,
    UnorderedMap<DexClass*, DFSStatus>& status,
    std::vector<DexClass*>& sorted,
    UnorderedSet<DexClass*>& in_cycle) const {
  auto it = status.find(cls);
  if (it == status.end() || it->second == DFSStatus::Unvisited) {
    status[cls] = DFSStatus::InProgress;

    // Visit all dependencies first
    auto deps_it = m_dependencies.find(cls);
    if (deps_it != m_dependencies.end()) {
      for (DexClass* dep : UnorderedIterable(deps_it->second)) {
        auto* cycle_root =
            topological_sort_visit(dep, status, sorted, in_cycle);
        if (cycle_root != nullptr) {
          // We're unwinding a cycle. Mark this node as cyclic.
          in_cycle.insert(cls);
          status[cls] = DFSStatus::Visited;
          if (cls == cycle_root) {
            // We've returned to the cycle root — stop propagating.
            return nullptr;
          }
          return cycle_root;
        }
      }
    }

    status[cls] = DFSStatus::Visited;
    sorted.push_back(cls);
    return nullptr;
  } else if (it->second == DFSStatus::InProgress) {
    // Cycle detected — this node is the cycle root.
    in_cycle.insert(cls);
    TRACE(CLINIT_BATCHING,
          1,
          "StaticFieldDependencyGraph: cycle detected involving %s",
          SHOW(cls));
    return cls;
  }
  // Already visited
  return nullptr;
}

TopologicalSortResult StaticFieldDependencyGraph::topological_sort() const {
  TopologicalSortResult result;

  if (m_classes.empty()) {
    return result;
  }

  UnorderedMap<DexClass*, DFSStatus> status;
  UnorderedSet<DexClass*> in_cycle;

  // Initialize all nodes as unvisited
  for (DexClass* cls : UnorderedIterable(m_classes)) {
    status[cls] = DFSStatus::Unvisited;
  }

  // Perform DFS from each unvisited node
  for (DexClass* cls : UnorderedIterable(m_classes)) {
    if (status[cls] == DFSStatus::Unvisited) {
      topological_sort_visit(cls, status, result.ordered_classes, in_cycle);
    }
  }

  // Move cyclic classes to a separate list
  if (!in_cycle.empty()) {
    // Cyclic nodes were not added to ordered_classes, so just copy from
    // in_cycle
    for (DexClass* cls : UnorderedIterable(in_cycle)) {
      result.cyclic_classes.push_back(cls);
    }
    result.cyclic_classes_count = in_cycle.size();

    TRACE(CLINIT_BATCHING,
          1,
          "StaticFieldDependencyGraph: found %zu classes in cycles, "
          "%zu classes in valid order",
          result.cyclic_classes.size(),
          result.ordered_classes.size());
  }

  return result;
}

} // namespace clinit_batching
