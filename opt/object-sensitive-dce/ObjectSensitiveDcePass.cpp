/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectSensitiveDcePass.h"

#include <fstream>
#include <functional>

#include "CFGMutation.h"
#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "HierarchyUtil.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "LocalPointersAnalysis.h"
#include "ObjectSensitiveDce.h"
#include "PassManager.h"
#include "Purity.h"
#include "ScopedCFG.h"
#include "SummarySerialization.h"
#include "Walkers.h"

namespace ptrs = local_pointers;

void ObjectSensitiveDcePass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  always_assert_log(
      !mgr.init_class_lowering_has_run(),
      "Implementation limitation: ObjectSensitiveDcePass could introduce new "
      "init-class instructions.");

  auto scope = build_class_scope(stores);
  auto method_override_graph = method_override_graph::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());

  auto pure_methods = get_pure_methods();
  auto configured_pure_methods = conf.get_pure_methods();
  pure_methods.insert(configured_pure_methods.begin(),
                      configured_pure_methods.end());
  auto immutable_getters = get_immutable_getters(scope);
  pure_methods.insert(immutable_getters.begin(), immutable_getters.end());

  ptrs::SummaryMap escape_summaries;
  if (m_external_escape_summaries_file) {
    std::ifstream file_input(*m_external_escape_summaries_file);
    summary_serialization::read(file_input, &escape_summaries);
  }
  mgr.incr_metric("external_escape_summaries", escape_summaries.size());

  side_effects::SummaryMap effect_summaries;
  if (m_external_side_effect_summaries_file) {
    std::ifstream file_input(*m_external_side_effect_summaries_file);
    summary_serialization::read(file_input, &effect_summaries);
  }
  mgr.incr_metric("external_side_effect_summaries", effect_summaries.size());

  ObjectSensitiveDce impl(scope,
                          &init_classes_with_side_effects,
                          pure_methods,
                          *method_override_graph,
                          m_big_override_threshold,
                          &escape_summaries,
                          &effect_summaries);
  impl.dce();

  auto& stats = impl.get_stats();
  auto invokes_with_summaries = stats.invokes_with_summaries;
  mgr.set_metric("removed_instructions", stats.removed_instructions);
  mgr.set_metric("init_class_instructions_added",
                 stats.init_class_instructions_added);
  mgr.incr_metric("init_class_instructions_removed",
                  stats.init_class_stats.init_class_instructions_removed);
  mgr.incr_metric("init_class_instructions_refined",
                  stats.init_class_stats.init_class_instructions_refined);
  mgr.set_metric("methods_with_summaries", stats.methods_with_summaries);
  mgr.set_metric("modified_params", stats.modified_params);
  mgr.set_metric("invoke_direct_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_DIRECT]);
  mgr.set_metric("invoke_static_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_STATIC]);
  mgr.set_metric("invoke_interface_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_INTERFACE]);
  mgr.set_metric("invoke_virtual_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_VIRTUAL]);
  mgr.set_metric("invoke_super_with_summaries",
                 invokes_with_summaries[OPCODE_INVOKE_SUPER]);
}

static ObjectSensitiveDcePass s_pass;
