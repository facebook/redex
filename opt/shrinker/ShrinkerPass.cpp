/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ShrinkerPass.h"

#include "ConfigFiles.h"
#include "ConstantPropagationAnalysis.h"
#include "PassManager.h"
#include "ScopedMetrics.h"
#include "Shrinker.h"
#include "Walkers.h"

void ShrinkerPass::bind_config() {
  bind("run_const_prop", true, m_config.run_const_prop,
       "Whether to run constant-propagation.");
  bind("run_cse", true, m_config.run_cse,
       "Whether to run common-subexpression-elimination.");
  bind("run_copy_prop", true, m_config.run_copy_prop,
       "Whether to run copy-propagation.");
  bind("run_local_dce", true, m_config.run_local_dce,
       "Whether to run local-dead-code-elimination.");
  bind("run_reg_alloc", false, m_config.run_reg_alloc,
       "Whether to run register allocation.");
  bind("run_fast_reg_alloc", false, m_config.run_fast_reg_alloc,
       "Whether to run fast register allocation.");
  bind("run_dedup_blocks", true, m_config.run_dedup_blocks,
       "Whether to run dedup-blocks.");
  bind("run_branch_prefix_hoisting", true, m_config.run_branch_prefix_hoisting,
       "Whether to run branch-prefix hoisting.");

  bind("compute_pure_methods", true, m_config.compute_pure_methods,
       "Whether to compute pure methods with a relatively expensive analysis "
       "over the scope.");

  bind("reg_alloc_random_forest", "", m_config.reg_alloc_random_forest,
       "Decide which functions to run register allocation on.");

  bind("analyze_constructors", false, m_config.analyze_constructors,
       "Whether to analyze constructors to find immutable attributes (only "
       "relevant when using constant-propagaation)");
}

void ShrinkerPass::eval_pass(DexStoresVector& /*stores*/,
                             ConfigFiles& /*conf*/,
                             PassManager&) {
  auto string_analyzer_state = constant_propagation::StringAnalyzerState::get();
  string_analyzer_state.set_methods_as_root();
}

void ShrinkerPass::run_pass(DexStoresVector& stores,
                            ConfigFiles& conf,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());

  int min_sdk = mgr.get_redex_options().min_sdk;
  shrinker::Shrinker shrinker(stores, scope, init_classes_with_side_effects,
                              m_config, min_sdk, conf.get_pure_methods(),
                              conf.get_finalish_field_names(), {},
                              mgr.get_redex_options().package_name);

  walk::parallel::code(scope, [&](DexMethod* method, IRCode&) {
    if (!method->rstate.no_optimizations()) {
      shrinker.shrink_method(method);
    }
  });

  mgr.incr_metric("methods_shrunk", shrinker.get_methods_shrunk());
  mgr.incr_metric(
      "instructions_eliminated_const_prop",
      shrinker.get_const_prop_stats().branches_removed +
          shrinker.get_const_prop_stats().unreachable_instructions_removed +
          shrinker.get_const_prop_stats().redundant_puts_removed +
          shrinker.get_const_prop_stats().branches_forwarded +
          shrinker.get_const_prop_stats().materialized_consts +
          shrinker.get_const_prop_stats().added_param_const +
          shrinker.get_const_prop_stats().throws +
          shrinker.get_const_prop_stats().null_checks);
  {
    ScopedMetrics sm(mgr);
    auto sm_scope = sm.scope("shrinker");
    shrinker.log_metrics(sm);
  }
  mgr.incr_metric("instructions_eliminated_cse",
                  shrinker.get_cse_stats().instructions_eliminated);
  mgr.incr_metric("instructions_eliminated_copy_prop",
                  shrinker.get_copy_prop_stats().moves_eliminated);
  mgr.incr_metric("instructions_eliminated_localdce_dead",
                  shrinker.get_local_dce_stats().dead_instruction_count);
  mgr.incr_metric("instructions_eliminated_localdce_unreachable",
                  shrinker.get_local_dce_stats().unreachable_instruction_count);
  mgr.incr_metric("instructions_eliminated_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().insns_removed);
  mgr.incr_metric("blocks_eliminated_by_dedup_blocks",
                  shrinker.get_dedup_blocks_stats().blocks_removed);
  mgr.incr_metric("instructions_eliminated_branch_prefix_hoisting",
                  shrinker.get_branch_prefix_hoisting_stats());
  mgr.incr_metric("methods_reg_alloced", shrinker.get_methods_reg_alloced());
  mgr.incr_metric("localdce_init_class_instructions_added",
                  shrinker.get_local_dce_stats().init_class_instructions_added);
  mgr.incr_metric(
      "localdce_init_class_instructions",
      shrinker.get_local_dce_stats().init_classes.init_class_instructions);
  mgr.incr_metric("localdce_init_class_instructions_removed",
                  shrinker.get_local_dce_stats()
                      .init_classes.init_class_instructions_removed);
  mgr.incr_metric("localdce_init_class_instructions_refined",
                  shrinker.get_local_dce_stats()
                      .init_classes.init_class_instructions_refined);

  // Expose the shrinking timers as Timers.
  Timer::add_timer("Shrinker.Shrinking.ConstantPropagation",
                   shrinker.get_const_prop_seconds());
  Timer::add_timer("Shrinker.Shrinking.CSE", shrinker.get_cse_seconds());
  Timer::add_timer("Shrinker.Shrinking.CopyPropagation",
                   shrinker.get_copy_prop_seconds());
  Timer::add_timer("Shrinker.Shrinking.LocalDCE",
                   shrinker.get_local_dce_seconds());
  Timer::add_timer("Shrinker.Shrinking.DedupBlocks",
                   shrinker.get_dedup_blocks_seconds());
  Timer::add_timer("Shrinker.Shrinking.BranchPrefixHoisting",
                   shrinker.get_branch_prefix_hoisting_seconds());
  Timer::add_timer("Shrinker.Shrinking.RegAlloc",
                   shrinker.get_reg_alloc_seconds());
}

static ShrinkerPass s_pass;
