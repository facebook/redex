/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include "Pass.h"
#include "Reachability.h"

class RemoveUnreachablePassBase : public Pass {
 public:
  explicit RemoveUnreachablePassBase(const std::string& name) : Pass(name) {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {UltralightCodePatterns, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override;
  void eval_pass(DexStoresVector& /*stores*/,
                 ConfigFiles& /*conf*/,
                 PassManager& /*mgr*/) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual std::unique_ptr<reachability::ReachableObjects>
  compute_reachable_objects(
      const Scope& scope,
      const method_override_graph::Graph& method_override_graph,
      PassManager& pm,
      int* num_ignore_check_strings,
      reachability::ReachableAspects* reachable_aspects,
      bool emit_graph_this_run,
      bool relaxed_keep_class_members,
      bool relaxed_keep_interfaces,
      bool cfg_gathering_check_instantiable,
      bool cfg_gathering_check_instance_callable,
      bool cfg_gathering_check_returning,
      bool remove_no_argument_constructors) = 0;

  void write_out_removed_symbols(
      const std::string& filepath,
      const ConcurrentSet<std::string>& removed_symbols);

 protected:
  virtual bool should_sweep_code() const {
    return m_prune_uninstantiable_insns || m_throw_propagation;
  }

  static reachability::ObjectCounts before_metrics(DexStoresVector& stores,
                                                   PassManager& pm);

  reachability::IgnoreSets m_ignore_sets;
  bool m_remove_no_argument_constructors = false;
  std::optional<uint32_t> m_emit_graph_on_run;
  bool m_always_emit_unreachable_symbols = false;
  bool m_emit_removed_symbols_references = false;
  bool m_output_full_removed_symbols = false;
  bool m_relaxed_keep_class_members = false;
  bool m_prune_uninstantiable_insns = false;
  bool m_prune_uncallable_instance_method_bodies = false;
  bool m_prune_uncallable_virtual_methods = false;
  bool m_prune_unreferenced_interfaces = false;
  bool m_throw_propagation = false;

  static bool s_emit_graph_on_last_run;
  static size_t s_all_reachability_runs;
  static size_t s_all_reachability_run;
};

class RemoveUnreachablePass : public RemoveUnreachablePassBase {
 public:
  RemoveUnreachablePass()
      : RemoveUnreachablePassBase("RemoveUnreachablePass") {}

  std::unique_ptr<reachability::ReachableObjects> compute_reachable_objects(
      const Scope& scope,
      const method_override_graph::Graph& method_override_graph,
      PassManager& pm,
      int* num_ignore_check_strings,
      reachability::ReachableAspects* reachable_aspects,
      bool emit_graph_this_run,
      bool relaxed_keep_class_members,
      bool relaxed_keep_interfaces,
      bool cfg_gathering_check_instantiable,
      bool cfg_gathering_check_instance_callable,
      bool cfg_gathering_check_returning,
      bool remove_no_argument_constructors) override;
};
