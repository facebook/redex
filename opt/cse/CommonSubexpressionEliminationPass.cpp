/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CommonSubexpressionEliminationPass.h"

#include "CommonSubexpressionElimination.h"
#include "ConfigFiles.h"
#include "CopyPropagation.h"
#include "DexUtil.h"
#include "LocalDce.h"
#include "Purity.h"
#include "Show.h"
#include "Walkers.h"

using namespace cse_impl;

namespace {

constexpr const char* METRIC_RESULTS_CAPTURED = "num_results_captured";
constexpr const char* METRIC_STORES_CAPTURED = "num_stores_captured";
constexpr const char* METRIC_ARRAY_LENGTHS_CAPTURED =
    "num_array_lengths_captured";
constexpr const char* METRIC_ELIMINATED_INSTRUCTIONS =
    "num_eliminated_instructions";
constexpr const char* METRIC_MAX_VALUE_IDS = "max_value_ids";
constexpr const char* METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT =
    "methods_using_other_tracked_location_bit";
constexpr const char* METRIC_INSTR_PREFIX = "instr_";
constexpr const char* METRIC_METHOD_BARRIERS = "num_method_barriers";
constexpr const char* METRIC_METHOD_BARRIERS_ITERATIONS =
    "num_method_barriers_iterations";
constexpr const char* METRIC_FINALIZABLE_FIELDS = "num_finalizable_fields";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS =
    "num_conditionally_pure_methods";
constexpr const char* METRIC_CONDITIONALLY_PURE_METHODS_ITERATIONS =
    "num_conditionally_pure_methods_iterations";
constexpr const char* METRIC_MAX_ITERATIONS = "num_max_iterations";

} // namespace

void CommonSubexpressionEliminationPass::bind_config() {
  bind("debug", false, m_debug);
  bind("runtime_assertions", false, m_runtime_assertions);
}

void CommonSubexpressionEliminationPass::run_pass(DexStoresVector& stores,
                                                  ConfigFiles& conf,
                                                  PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());

  auto pure_methods = /* Android framework */ get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());
  auto immutable_getters = get_immutable_getters(scope);
  pure_methods.insert(immutable_getters.begin(), immutable_getters.end());

  std::unordered_set<const DexField*> finalish_fields;
  auto shared_state = SharedState(
      pure_methods, conf.get_finalish_field_names(), finalish_fields);
  method::ClInitHasNoSideEffectsPredicate clinit_has_no_side_effects =
      [&](const DexType* type) {
        return !init_classes_with_side_effects.refine(type);
      };
  shared_state.init_scope(scope, clinit_has_no_side_effects);

  // The following default 'features' of copy propagation would only
  // interfere with what CSE is trying to do.
  copy_propagation_impl::Config copy_prop_config;
  copy_prop_config.eliminate_const_classes = false;
  copy_prop_config.eliminate_const_strings = false;
  copy_prop_config.static_finals = false;
  const auto stats = walk::parallel::methods<Stats>(
      scope,
      [&](DexMethod* method) {
        const auto code = method->get_code();
        if (code == nullptr) {
          return Stats();
        }

        if (method->rstate.no_optimizations()) {
          return Stats();
        }

        Stats stats;
        while (true) {
          stats.max_iterations++;
          TRACE(CSE, 3, "[CSE] processing %s", SHOW(method));
          always_assert(code->editable_cfg_built());
          CommonSubexpressionElimination cse(
              &shared_state, code->cfg(), is_static(method),
              method::is_init(method) || method::is_clinit(method),
              method->get_class(), method->get_proto()->get_args());
          bool any_changes = cse.patch(m_runtime_assertions);
          stats += cse.get_stats();

          if (!any_changes) {
            return stats;
          }

          copy_propagation_impl::CopyPropagation copy_propagation(
              copy_prop_config);
          copy_propagation.run(code, method);

          auto local_dce = LocalDce(&init_classes_with_side_effects,
                                    shared_state.get_pure_methods(),
                                    shared_state.get_method_override_graph(),
                                    /* may_allocate_registers */ true);
          local_dce.dce(
              code, /* normalize_new_instances */ true, method->get_class());

          if (traceEnabled(CSE, 5)) {
            TRACE(CSE, 5, "[CSE] end of iteration:\n%s", SHOW(code->cfg()));
          }
        }
      },
      m_debug ? 1 : redex_parallel::default_num_threads());
  mgr.incr_metric(METRIC_RESULTS_CAPTURED, stats.results_captured);
  mgr.incr_metric(METRIC_STORES_CAPTURED, stats.stores_captured);
  mgr.incr_metric(METRIC_ARRAY_LENGTHS_CAPTURED, stats.array_lengths_captured);
  mgr.incr_metric(METRIC_ELIMINATED_INSTRUCTIONS,
                  stats.instructions_eliminated);
  mgr.incr_metric(METRIC_MAX_VALUE_IDS, stats.max_value_ids);
  mgr.incr_metric(METRIC_METHODS_USING_OTHER_TRACKED_LOCATION_BIT,
                  stats.methods_using_other_tracked_location_bit);
  auto& shared_state_stats = shared_state.get_stats();
  mgr.incr_metric(METRIC_METHOD_BARRIERS, shared_state_stats.method_barriers);
  mgr.incr_metric(METRIC_METHOD_BARRIERS_ITERATIONS,
                  shared_state_stats.method_barriers_iterations);
  mgr.incr_metric(METRIC_FINALIZABLE_FIELDS,
                  shared_state_stats.finalizable_fields);
  mgr.incr_metric(METRIC_CONDITIONALLY_PURE_METHODS,
                  shared_state_stats.conditionally_pure_methods);
  mgr.incr_metric(METRIC_CONDITIONALLY_PURE_METHODS_ITERATIONS,
                  shared_state_stats.conditionally_pure_methods_iterations);
  for (auto& p : stats.eliminated_opcodes) {
    std::string name = METRIC_INSTR_PREFIX;
    name += SHOW(static_cast<IROpcode>(p.first));
    mgr.incr_metric(name, p.second);
  }
  mgr.incr_metric(METRIC_MAX_ITERATIONS, stats.max_iterations);

  shared_state.cleanup();
}

static CommonSubexpressionEliminationPass s_pass;
