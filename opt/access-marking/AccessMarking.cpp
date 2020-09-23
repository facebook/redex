/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AccessMarking.h"

#include <unordered_map>

#include "ClassHierarchy.h"
#include "DexUtil.h"
#include "FieldOpTracker.h"
#include "IRCode.h"
#include "MethodOverrideGraph.h"
#include "Mutators.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

size_t mark_classes_final(const Scope& scope) {
  ClassHierarchy ch = build_type_hierarchy(scope);
  size_t n_classes_finalized = 0;
  for (auto const& cls : scope) {
    if (!can_rename(cls) || is_abstract(cls) || is_final(cls)) {
      continue;
    }
    auto const& children = get_children(ch, cls->get_type());
    if (children.empty()) {
      TRACE(ACCESS, 2, "Finalizing class: %s", SHOW(cls));
      set_final(cls);
      ++n_classes_finalized;
    }
  }
  return n_classes_finalized;
}

size_t mark_methods_final(const Scope& scope,
                          const mog::Graph& override_graph) {
  size_t n_methods_finalized = 0;
  for (auto const& cls : scope) {
    for (auto const& method : cls->get_vmethods()) {
      if (!can_rename(method) || is_abstract(method) || is_final(method)) {
        continue;
      }
      if (override_graph.get_node(method).children.empty()) {
        TRACE(ACCESS, 2, "Finalizing method: %s", SHOW(method));
        set_final(method);
        ++n_methods_finalized;
      }
    }
  }
  return n_methods_finalized;
}

size_t mark_fields_final(const Scope& scope) {
  field_op_tracker::FieldStatsMap field_stats =
      field_op_tracker::analyze(scope);

  size_t n_fields_finalized = 0;
  for (auto& pair : field_stats) {
    auto* field = pair.first;
    auto& stats = pair.second;
    if (stats.writes == 0 && !is_final(field) && !is_volatile(field) &&
        !field->is_external()) {
      set_final(field);
      ++n_fields_finalized;
    }
  }
  return n_fields_finalized;
}

std::vector<DexMethod*> direct_methods(const std::vector<DexClass*>& scope) {
  std::vector<DexMethod*> ret;
  for (auto cls : scope) {
    for (auto m : cls->get_dmethods()) {
      ret.push_back(m);
    }
  }
  return ret;
}

std::unordered_set<DexMethod*> find_private_methods(
    const std::vector<DexClass*>& scope, const mog::Graph& override_graph) {
  auto candidates = mog::get_non_true_virtuals(override_graph, scope);
  auto dmethods = direct_methods(scope);
  for (auto* dmethod : dmethods) {
    candidates.emplace(dmethod);
  }
  std20::erase_if(candidates, [](auto it) {
    auto* m = *it;
    TRACE(ACCESS, 3, "Considering for privatization: %s", SHOW(m));
    return method::is_clinit(m) || !can_rename(m) || is_abstract(m) ||
           is_private(m);
  });

  ConcurrentSet<DexMethod*> externally_referenced;
  walk::parallel::opcodes(
      scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* caller, IRInstruction* inst) {
        if (!inst->has_method()) {
          return;
        }
        auto callee =
            resolve_method(inst->get_method(), opcode_to_search(inst), caller);
        if (callee == nullptr || callee->get_class() == caller->get_class()) {
          return;
        }
        externally_referenced.emplace(callee);
      });

  for (auto* m : externally_referenced) {
    candidates.erase(m);
  }
  return candidates;
}

void fix_call_sites_private(const std::vector<DexClass*>& scope,
                            const std::unordered_set<DexMethod*>& privates) {
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    for (const MethodItemEntry& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      if (!insn->has_method()) continue;
      auto callee =
          resolve_method(insn->get_method(), opcode_to_search(insn), caller);
      // should be safe to read `privates` here because there are no writers
      if (callee != nullptr && privates.count(callee)) {
        insn->set_method(callee);
        if (!is_static(callee)) {
          insn->set_opcode(OPCODE_INVOKE_DIRECT);
        }
      }
    }
  });
}

void mark_methods_private(const std::unordered_set<DexMethod*>& privates) {
  // Compute an ordered representation of the methods. This matters, as
  // the dmethods and vmethods are not necessarily sorted, but add_method does
  // a best-effort of inserting in an ordered matter.
  // But when dmethods and vmethods are not ordered to begin with, then the
  // order in which we attempt to add matters.
  std::vector<DexMethod*> ordered_privates(privates.begin(), privates.end());
  std::sort(
      ordered_privates.begin(), ordered_privates.end(), compare_dexmethods);

  for (auto method : ordered_privates) {
    TRACE(ACCESS, 2, "Privatized method: %s", SHOW(method));
    auto cls = type_class(method->get_class());
    cls->remove_method(method);
    method->set_virtual(false);
    set_private(method);
    cls->add_method(method);
  }
}
} // namespace

void AccessMarkingPass::run_pass(DexStoresVector& stores,
                                 ConfigFiles& /* conf */,
                                 PassManager& pm) {
  auto scope = build_class_scope(stores);
  auto override_graph = mog::build_graph(scope);
  if (m_finalize_classes) {
    auto n_classes_final = mark_classes_final(scope);
    pm.incr_metric("finalized_classes", n_classes_final);
    TRACE(ACCESS, 1, "Finalized %lu classes", n_classes_final);
  }
  if (m_finalize_methods) {
    auto n_methods_final = mark_methods_final(scope, *override_graph);
    pm.incr_metric("finalized_methods", n_methods_final);
    TRACE(ACCESS, 1, "Finalized %lu methods", n_methods_final);
  }
  if (m_finalize_fields) {
    auto n_fields_final = mark_fields_final(scope);
    pm.incr_metric("finalized_fields", n_fields_final);
    TRACE(ACCESS, 1, "Finalized %lu fields", n_fields_final);
  }
  if (m_privatize_methods) {
    auto privates = find_private_methods(scope, *override_graph);
    fix_call_sites_private(scope, privates);
    mark_methods_private(privates);
    pm.incr_metric("privatized_methods", privates.size());
    TRACE(ACCESS, 1, "Privatized %lu methods", privates.size());
  }
}

static AccessMarkingPass s_pass;
