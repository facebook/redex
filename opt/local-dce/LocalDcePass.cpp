/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalDcePass.h"

#include <array>
#include <iostream>
#include <unordered_set>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodOverrideGraph.h"
#include "Purity.h"
#include "Resolver.h"
#include "Transform.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_DEAD_INSTRUCTIONS = "num_dead_instructions";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "num_unreachable_instructions";
constexpr const char* METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS =
    "num_computed_no_side_effects_methods";
constexpr const char* METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS_ITERATIONS =
    "num_computed_no_side_effects_methods_iterations";

ConcurrentSet<DexMethod*> get_no_implementor_abstract_methods(
    const Scope& scope) {
  ConcurrentSet<DexMethod*> method_set;
  ClassScopes cs(scope);
  TypeSystem ts(scope);
  // Find non-interface abstract methods that have no implementation.
  walk::parallel::methods(scope, [&method_set, &cs](DexMethod* method) {
    DexClass* method_cls = type_class(method->get_class());
    if (!is_abstract(method) || method->is_external() || method->get_code() ||
        !method_cls || is_interface(method_cls)) {
      return;
    }
    const auto& virtual_scope = cs.find_virtual_scope(method);
    if (virtual_scope.methods.size() == 1 && !root(method)) {
      always_assert_log(virtual_scope.methods[0].first == method,
                        "LocalDCE: abstract method not in its virtual scope, "
                        "virtual scope must be problematic");
      method_set.emplace(method);
    }
  });

  // Find methods of interfaces that have no implementor/interface children.
  walk::parallel::classes(scope, [&method_set, &ts](DexClass* cls) {
    if (!is_interface(cls) || cls->is_external()) {
      return;
    }
    DexType* cls_type = cls->get_type();
    if (ts.get_implementors(cls_type).size() == 0 &&
        ts.get_interface_children(cls_type).size() == 0) {
      for (auto method : cls->get_vmethods()) {
        if (is_abstract(method)) {
          method_set.emplace(method);
        }
      }
    }
  });
  return method_set;
}
} // namespace

void LocalDcePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto pure_methods = find_pure_methods(scope);
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());
  auto override_graph = method_override_graph::build_graph(scope);
  std::unordered_set<const DexMethod*> computed_no_side_effects_methods;
  auto computed_no_side_effects_methods_iterations =
      compute_no_side_effects_methods(scope, override_graph.get(), pure_methods,
                                      &computed_no_side_effects_methods);
  for (auto m : computed_no_side_effects_methods) {
    pure_methods.insert(const_cast<DexMethod*>(m));
  }

  auto stats =
      walk::parallel::methods<LocalDce::Stats>(scope, [&](DexMethod* m) {
        auto* code = m->get_code();
        if (code == nullptr || m->rstate.no_optimizations()) {
          return LocalDce::Stats();
        }

        LocalDce ldce(pure_methods);
        ldce.dce(code);
        return ldce.get_stats();
      });
  mgr.incr_metric(METRIC_DEAD_INSTRUCTIONS, stats.dead_instruction_count);
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  stats.unreachable_instruction_count);
  mgr.incr_metric(METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS,
                  computed_no_side_effects_methods.size());
  mgr.incr_metric(METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS_ITERATIONS,
                  computed_no_side_effects_methods_iterations);

  TRACE(DCE, 1, "instructions removed -- dead: %d, unreachable: %d",
        stats.dead_instruction_count, stats.unreachable_instruction_count);
}

std::unordered_set<DexMethodRef*> LocalDcePass::find_pure_methods(
    const Scope& scope) {
  auto pure_methods = get_pure_methods();
  if (no_implementor_abstract_is_pure) {
    // Find abstract methods that have no implementors
    ConcurrentSet<DexMethod*> concurrent_method_set =
        get_no_implementor_abstract_methods(scope);
    for (auto method : concurrent_method_set) {
      pure_methods.emplace(method);
    }
  }
  return pure_methods;
}

static LocalDcePass s_pass;
