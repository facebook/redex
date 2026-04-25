/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "Pass.h"
#include "StaticFieldDependencyGraph.h"

/**
 * ClinitBatchingPass eliminates class initializers (clinits) by making classes
 * "trivially initialized" and batching all static field initialization into a
 * single early-startup method that can be AOT-compiled.
 *
 * This pass:
 * 1. Identifies hot clinits based on baseline profile data
 * 2. Extracts each clinit body into a new __initStatics$<ClassName>() method
 * 3. Removes the original clinit (making the class trivially initialized)
 * 4. Generates an orchestrator method that calls all __initStatics$*() methods
 *    in dependency order
 *
 * Configuration:
 * - interaction_pattern: Regex to filter baseline profile interactions
 */
class ClinitBatchingPass : public Pass {
 public:
  ClinitBatchingPass() : Pass("ClinitBatchingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Destroys},
        {NoResolvablePureRefs, Preserves},
        {HasSourceBlocks, Preserves},
    };
  }

  std::string get_config_doc() override {
    return "Eliminates class initializers (clinits) by extracting their bodies "
           "into new __initStatics$*() methods and batching all initialization "
           "into a single early-startup orchestrator method. Targets hot "
           "clinits identified via baseline profile data.";
  }

  void bind_config() override;

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  /**
   * Identifies candidate clinits for batching based on baseline profile data.
   * A clinit is a candidate if:
   * 1. It is marked as hot in the baseline profile
   * 2. It doesn't have no_optimizations or should_not_outline flags
   * 3. Its class is not a reachability root (can be deleted)
   * 4. Its class can be renamed (not referenced externally via JNI/reflection)
   * 5. Its class is not an enum (TODO: verify whether enums actually need
   *    special treatment — see comment in implementation)
   *
   * @param scope The class scope to analyze
   * @param conf Configuration files containing baseline profile info
   * @param mgr Pass manager for metrics
   * @return Map of clinit methods to their containing classes
   */
  InsertOnlyConcurrentMap<DexMethod*, DexClass*> identify_candidate_clinits(
      const Scope& scope, ConfigFiles& conf, PassManager& mgr);

  /**
   * Builds a dependency graph between candidate classes based on static field
   * accesses, static method invocations, and new instance creations in clinits.
   * Performs topological sort and reports metrics.
   *
   * @param candidate_clinits Map of clinit methods to their containing classes
   * @param mgr Pass manager for metrics
   * @return Topological sort result with ordered and cyclic classes
   */
  clinit_batching::TopologicalSortResult build_dependency_graph(
      const UnorderedMap<DexMethod*, DexClass*>& candidate_clinits,
      PassManager& mgr,
      const method_override_graph::Graph* override_graph = nullptr,
      bool skip_benign = false);

  /**
   * Transforms candidate clinits by:
   * 1. Removing ACC_FINAL from static fields written by the clinit
   * 2. Creating a new __initStatics$<ClassName>() method with the clinit body
   * 3. Removing the original clinit (making the class trivially initialized)
   *
   * @param ordered_classes Classes in dependency order for initialization
   * @param conf Configuration files for method profiles
   * @param mgr Pass manager for metrics
   * @return Map of classes to their new __initStatics$*() methods
   */
  UnorderedMap<DexClass*, DexMethod*> transform_clinits(
      const std::vector<DexClass*>& ordered_classes,
      ConfigFiles& conf,
      PassManager& mgr);

  /**
   * Generates the orchestrator method body that calls all __initStatics$*()
   * methods in dependency order.
   *
   * @param orchestrator_method The annotated method to fill with calls
   * @param init_statics_methods Map of classes to their __initStatics$*()
   * methods
   * @param ordered_classes Classes in dependency order for initialization
   * @param mgr Pass manager for metrics
   * @return Number of init calls generated
   */
  size_t generate_orchestrator(
      DexMethod* orchestrator_method,
      const UnorderedMap<DexClass*, DexMethod*>& init_statics_methods,
      const std::vector<DexClass*>& ordered_classes,
      PassManager& mgr);

  std::string m_interaction_pattern;
  std::string m_orchestrator_annotation;
  bool m_skip_benign_virtual_calls{false};

  // Method annotated with @GenerateStaticInitBatch, found during eval_pass
  DexMethod* m_orchestrator_method{nullptr};
};
