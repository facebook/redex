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

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Purity.h"
#include "Resolver.h"
#include "Trace.h"
#include "Transform.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_NPE_INSTRUCTIONS = "num_npe_instructions";
constexpr const char* METRIC_DEAD_INSTRUCTIONS = "num_dead_instructions";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "num_unreachable_instructions";
constexpr const char* METRIC_NORMALIZED_NEW_INSTANCES =
    "num_normalized_new_instances";
constexpr const char* METRIC_ALIASED_NEW_INSTANCES =
    "num_aliased_new_instances";
constexpr const char* METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS =
    "num_computed_no_side_effects_methods";
constexpr const char* METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS_ITERATIONS =
    "num_computed_no_side_effects_methods_iterations";

} // namespace

void LocalDcePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto pure_methods = get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());
  auto immutable_getters = get_immutable_getters(scope);
  pure_methods.insert(immutable_getters.begin(), immutable_getters.end());
  auto override_graph = method_override_graph::build_graph(scope);
  std::unordered_set<const DexMethod*> computed_no_side_effects_methods;
  auto computed_no_side_effects_methods_iterations =
      compute_no_side_effects_methods(scope, override_graph.get(), pure_methods,
                                      &computed_no_side_effects_methods);
  for (auto m : computed_no_side_effects_methods) {
    pure_methods.insert(const_cast<DexMethod*>(m));
  }

  bool may_allocate_registers = !mgr.regalloc_has_run();
  if (!may_allocate_registers) {
    // compute_no_side_effects_methods might have found methods that have no
    // implementors. Let's not silently remove invocations to those in LocalDce,
    // as invoking them *will* unconditionally cause an exception.
    for (auto it = pure_methods.begin(); it != pure_methods.end();) {
      auto m = *it;
      if (m->is_def() && !has_implementor(override_graph.get(), m->as_def())) {
        it = pure_methods.erase(it);
      } else {
        it++;
      }
    }
  }

  auto stats =
      walk::parallel::methods<LocalDce::Stats>(scope, [&](DexMethod* m) {
        auto* code = m->get_code();
        if (code == nullptr || m->rstate.no_optimizations()) {
          return LocalDce::Stats();
        }

        LocalDce ldce(pure_methods, override_graph.get(),
                      may_allocate_registers);
        ldce.dce(code);
        return ldce.get_stats();
      });
  mgr.incr_metric(METRIC_NPE_INSTRUCTIONS, stats.npe_instruction_count);
  mgr.incr_metric(METRIC_DEAD_INSTRUCTIONS, stats.dead_instruction_count);
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  stats.unreachable_instruction_count);
  mgr.incr_metric(METRIC_NORMALIZED_NEW_INSTANCES,
                  stats.normalized_new_instances);
  mgr.incr_metric(METRIC_ALIASED_NEW_INSTANCES, stats.aliased_new_instances);
  mgr.incr_metric(METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS,
                  computed_no_side_effects_methods.size());
  mgr.incr_metric(METRIC_COMPUTED_NO_SIDE_EFFECTS_METHODS_ITERATIONS,
                  computed_no_side_effects_methods_iterations);

  TRACE(DCE, 1,
        "instructions removed -- npe: %zu, dead: %zu, unreachable: %zu; "
        "normalized %zu new-instance instructions, %zu aliasaed",
        stats.npe_instruction_count, stats.dead_instruction_count,
        stats.unreachable_instruction_count, stats.normalized_new_instances,
        stats.aliased_new_instances);
}

static LocalDcePass s_pass;
