/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ABExperimentContext.h"
#include "DexStore.h"
#include "FrameworkApi.h"
#include "InitClassesWithSideEffects.h"
#include "InlinerConfig.h"
#include "Pass.h"
#include "Resolver.h"
#include "TypeSystem.h"

class MultiMethodInliner;

namespace method_profiles {
class MethodProfiles;
} // namespace method_profiles

struct VirtualMergingStats {
  size_t invoke_super_methods{0};
  size_t invoke_super_methods_refs{0};
  size_t invoke_super_unresolved_method_refs{0};
  size_t mergeable_virtual_methods{0};
  size_t annotated_methods{0};
  size_t cross_store_refs{0};
  size_t cross_dex_refs{0};
  size_t unavailable_overridden_methods{0};
  size_t inconcrete_overridden_methods{0};
  size_t abstract_overridden_methods{0};
  size_t mergeable_scope_methods{0};
  size_t mergeable_pairs{0};
  size_t virtual_scopes_with_mergeable_pairs{0};
  size_t unabstracted_methods{0};
  size_t uninlinable_methods{0};
  size_t huge_methods{0};
  size_t caller_size_removed_methods{0};
  size_t removed_virtual_methods{0};
  size_t experiment_methods{0};

  VirtualMergingStats& operator+=(const VirtualMergingStats& rhs) {
    invoke_super_methods += rhs.invoke_super_methods;
    invoke_super_methods_refs += rhs.invoke_super_methods_refs;
    invoke_super_unresolved_method_refs +=
        rhs.invoke_super_unresolved_method_refs;
    mergeable_virtual_methods += rhs.mergeable_virtual_methods;
    annotated_methods += rhs.annotated_methods;
    cross_store_refs += rhs.cross_store_refs;
    cross_dex_refs += rhs.cross_dex_refs;
    unavailable_overridden_methods += rhs.unavailable_overridden_methods;
    inconcrete_overridden_methods += rhs.inconcrete_overridden_methods;
    abstract_overridden_methods += rhs.abstract_overridden_methods;
    mergeable_scope_methods += rhs.mergeable_scope_methods;
    mergeable_pairs += rhs.mergeable_pairs;
    virtual_scopes_with_mergeable_pairs +=
        rhs.virtual_scopes_with_mergeable_pairs;
    unabstracted_methods += rhs.unabstracted_methods;
    uninlinable_methods += rhs.uninlinable_methods;
    huge_methods += rhs.huge_methods;
    caller_size_removed_methods += rhs.caller_size_removed_methods;
    removed_virtual_methods += rhs.removed_virtual_methods;
    experiment_methods += rhs.experiment_methods;
    return *this;
  }

  bool operator==(const VirtualMergingStats& rhs) {
    return invoke_super_methods == rhs.invoke_super_methods &&
           invoke_super_methods_refs == rhs.invoke_super_methods_refs &&
           invoke_super_unresolved_method_refs ==
               rhs.invoke_super_unresolved_method_refs &&
           mergeable_virtual_methods == rhs.mergeable_virtual_methods &&
           annotated_methods == rhs.annotated_methods &&
           cross_store_refs == rhs.cross_store_refs &&
           cross_dex_refs == rhs.cross_dex_refs &&
           unavailable_overridden_methods ==
               rhs.unavailable_overridden_methods &&
           inconcrete_overridden_methods == rhs.inconcrete_overridden_methods &&
           abstract_overridden_methods == rhs.abstract_overridden_methods &&
           mergeable_scope_methods == rhs.mergeable_scope_methods &&
           mergeable_pairs == rhs.mergeable_pairs &&
           virtual_scopes_with_mergeable_pairs ==
               rhs.virtual_scopes_with_mergeable_pairs &&
           unabstracted_methods == rhs.unabstracted_methods &&
           uninlinable_methods == rhs.uninlinable_methods &&
           huge_methods == rhs.huge_methods &&
           caller_size_removed_methods == rhs.caller_size_removed_methods &&
           removed_virtual_methods == rhs.removed_virtual_methods &&
           experiment_methods == rhs.experiment_methods;
  }
};

class VirtualMerging {
 public:
  enum class Strategy {
    kLexicographical,
    kProfileCallCount,
    kProfileAppearBucketsAndCallCount,
  };

  enum class InsertionStrategy {
    kJumpTo,
    kFallthrough,
  };

  VirtualMerging(DexStoresVector&,
                 const inliner::InlinerConfig&,
                 size_t,
                 const api::AndroidSDK* min_sdk_api = nullptr);
  ~VirtualMerging();
  void run(const method_profiles::MethodProfiles&,
           Strategy strategy,
           InsertionStrategy insertion_strategy,
           Strategy ab_strategy = Strategy::kLexicographical,
           ab_test::ABExperimentContext* ab_experiment_context = nullptr);
  const VirtualMergingStats& get_stats() { return m_stats; }

 private:
  Scope m_scope;
  XStoreRefs m_xstores;
  XDexRefs m_xdexes;
  TypeSystem m_type_system;
  size_t m_max_overriding_method_instructions;
  ConcurrentMethodRefCache m_concurrent_resolved_refs;
  inliner::InlinerConfig m_inliner_config;
  init_classes::InitClassesWithSideEffects m_init_classes_with_side_effects;

  std::unique_ptr<MultiMethodInliner> m_inliner;
  VirtualMergingStats m_stats;

  void find_unsupported_virtual_scopes();
  std::unordered_set<const VirtualScope*> m_unsupported_virtual_scopes;
  std::unordered_map<const DexString*, std::unordered_set<DexProto*>>
      m_unsupported_named_protos;

  void compute_mergeable_scope_methods();
  ConcurrentMap<const VirtualScope*, std::unordered_set<const DexMethod*>>
      m_mergeable_scope_methods;

 public:
  using MergablePairsByVirtualScope =
      std::map<const VirtualScope*,
               std::vector<std::pair<const DexMethod*, const DexMethod*>>,
               virtualscopes_comparator>;

 private:
  MergablePairsByVirtualScope compute_mergeable_pairs_by_virtual_scopes(
      const method_profiles::MethodProfiles&,
      Strategy strategy,
      VirtualMergingStats&) const;

  void merge_methods(const MergablePairsByVirtualScope& mergable_pairs,
                     const MergablePairsByVirtualScope& exp_mergable_pairs,
                     ab_test::ABExperimentContext* ab_experiment_context,
                     InsertionStrategy insertion_strategy);
  std::unordered_map<DexClass*, std::vector<const DexMethod*>>
      m_virtual_methods_to_remove;
  std::unordered_map<DexMethod*, DexMethod*> m_virtual_methods_to_remap;

  void remove_methods();

  void remap_invoke_virtuals();
};

class VirtualMergingPass : public Pass {
 public:
  VirtualMergingPass() : Pass("VirtualMergingPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  int64_t m_max_overriding_method_instructions;
  VirtualMerging::Strategy m_strategy{
      VirtualMerging::Strategy::kProfileCallCount};
  VirtualMerging::Strategy m_ab_strategy{
      VirtualMerging::Strategy::kLexicographical};
};
