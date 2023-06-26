/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
        {HasSourceBlocks, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
        {UltralightCodePatterns, Preserves},
    };
  }

  void bind_config() override {
    bind("ignore_string_literals", {}, m_ignore_sets.string_literals);
    bind("ignore_string_literal_annos", {}, m_ignore_sets.string_literal_annos);
    bind("ignore_system_annos", {}, m_ignore_sets.system_annos);
    bind("keep_class_in_string", true, m_ignore_sets.keep_class_in_string);
    bind("emit_graph_on_run", boost::optional<uint32_t>{}, m_emit_graph_on_run);
    bind("always_emit_unreachable_symbols",
         false,
         m_always_emit_unreachable_symbols);
    // This config allows unused constructors without argument to be removed.
    // This is only used for testing in microbenchmarks.
    bind("remove_no_argument_constructors",
         false,
         m_remove_no_argument_constructors);
    bind("output_full_removed_symbols", false, m_output_full_removed_symbols);
    bind("relaxed_keep_class_members", false, m_relaxed_keep_class_members);
    bind("prune_uninstantiable_insns", false, m_prune_uninstantiable_insns);
    after_configuration([this] {
      // To keep the backward compatability of this code, ensure that the
      // "MemberClasses" annotation is always in system_annos.
      m_ignore_sets.system_annos.emplace(
          DexType::get_type("Ldalvik/annotation/MemberClasses;"));
    });
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual std::unique_ptr<reachability::ReachableObjects>
  compute_reachable_objects(const DexStoresVector& stores,
                            PassManager& pm,
                            int* num_ignore_check_strings,
                            reachability::ReachableAspects* reachable_aspects,
                            bool emit_graph_this_run,
                            bool relaxed_keep_class_members,
                            bool cfg_gathering_check_instantiable,
                            bool remove_no_argument_constructors) = 0;

  void write_out_removed_symbols(
      const std::string& filepath,
      const ConcurrentSet<std::string>& removed_symbols);

 protected:
  reachability::IgnoreSets m_ignore_sets;
  bool m_remove_no_argument_constructors = false;
  boost::optional<uint32_t> m_emit_graph_on_run;
  bool m_always_emit_unreachable_symbols = false;
  bool m_emit_removed_symbols_references = false;
  bool m_output_full_removed_symbols = false;
  bool m_relaxed_keep_class_members = false;
  bool m_prune_uninstantiable_insns = false;
};

class RemoveUnreachablePass : public RemoveUnreachablePassBase {
 public:
  RemoveUnreachablePass()
      : RemoveUnreachablePassBase("RemoveUnreachablePass") {}

  std::unique_ptr<reachability::ReachableObjects> compute_reachable_objects(
      const DexStoresVector& stores,
      PassManager& pm,
      int* num_ignore_check_strings,
      reachability::ReachableAspects* reachable_aspects,
      bool emit_graph_this_run,
      bool relaxed_keep_class_members,
      bool cfg_gathering_check_instantiable,
      bool remove_no_argument_constructors) override;
};
