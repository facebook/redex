/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "InitClassesWithSideEffects.h"
#include "LocalDce.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"

namespace remove_unused_args {

namespace mog = method_override_graph;

std::deque<uint16_t> compute_live_args(
    DexMethod* method,
    size_t num_args,
    std::vector<cfg::InstructionIterator>* dead_insns);

class RemoveArgs {
 public:
  struct MethodStats {
    size_t method_params_removed_count{0};
    size_t method_results_removed_count{0};
    size_t method_protos_reordered_count{0};
    size_t methods_updated_count{0};
    LocalDce::Stats local_dce_stats;
  };
  struct PassStats {
    size_t method_params_removed_count{0};
    size_t methods_updated_count{0};
    size_t callsite_args_removed_count{0};
    size_t method_results_removed_count{0};
    size_t method_protos_reordered_count{0};
    LocalDce::Stats local_dce_stats;
  };

  RemoveArgs(const Scope& scope,
             const init_classes::InitClassesWithSideEffects&
                 init_classes_with_side_effects,
             const std::vector<std::string>& blocklist,
             size_t iteration = 0)
      : m_scope(scope),
        m_init_classes_with_side_effects(init_classes_with_side_effects),
        m_blocklist(blocklist),
        m_iteration(iteration) {}
  RemoveArgs::PassStats run(ConfigFiles& conf);

 private:
  const Scope& m_scope;
  const init_classes::InitClassesWithSideEffects&
      m_init_classes_with_side_effects;
  ConcurrentMap<DexMethod*, std::deque<uint16_t>> m_live_arg_idxs_map;
  // Data structure to remember running indices to make method names unique when
  // we reorder prototypes across virtual scopes, or do other general changes to
  // non-virtuals.
  struct NamedRenameMap {
    size_t next_reordering_uniquifiers{0};
    std::unordered_map<DexTypeList*, size_t> reordering_uniquifiers;
    std::unordered_map<DexTypeList*, size_t> general_uniquifiers;
  };
  std::unordered_map<const DexString*, NamedRenameMap> m_rename_maps;
  ConcurrentSet<DexMethod*> m_result_used;
  std::unordered_map<DexProto*, DexProto*> m_reordered_protos;
  const std::vector<std::string>& m_blocklist;
  size_t m_iteration;

  DexTypeList::ContainerType get_live_arg_type_list(
      DexMethod* method, const std::deque<uint16_t>& live_arg_idxs);
  bool update_method_signature(DexMethod* method,
                               const std::deque<uint16_t>& live_arg_idxs,
                               bool remove_result,
                               DexProto* reordered_proto);
  MethodStats update_method_protos(
      const mog::Graph& override_graph,
      const std::unordered_set<DexType*>& no_devirtualize_anno);
  size_t update_callsite(IRInstruction* instr);
  std::pair<size_t, LocalDce::Stats> update_callsites();
  void gather_results_used();
  void compute_reordered_protos(const mog::Graph& override_graph);
};

class RemoveUnusedArgsPass : public Pass {
 public:
  RemoveUnusedArgsPass() : Pass("RemoveUnusedArgsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override { bind("blocklist", {}, m_blocklist); }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager& mgr) override;

 private:
  std::vector<std::string> m_blocklist;
  size_t m_total_iterations{0};
};

} // namespace remove_unused_args
