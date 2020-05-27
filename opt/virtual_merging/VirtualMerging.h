/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Inliner.h"
#include "Pass.h"
#include "PassManager.h"
#include "TypeSystem.h"

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
  size_t removed_virtual_methods{0};
};

class VirtualMerging {
 public:
  VirtualMerging(DexStoresVector&, const inliner::InlinerConfig&, size_t);
  void run(const method_profiles::MethodProfiles&);
  const VirtualMergingStats& get_stats() { return m_stats; }

 private:
  Scope m_scope;
  XStoreRefs m_xstores;
  XDexRefs m_xdexes;
  TypeSystem m_type_system;
  size_t m_max_overriding_method_instructions;
  MethodRefCache m_resolved_refs;
  inliner::InlinerConfig m_inliner_config;
  std::unique_ptr<MultiMethodInliner> m_inliner;
  VirtualMergingStats m_stats;

  void find_unsupported_virtual_scopes();
  std::unordered_set<const VirtualScope*> m_unsupported_virtual_scopes;
  std::unordered_map<DexString*, std::unordered_set<DexProto*>>
      m_unsupported_named_protos;

  void compute_mergeable_scope_methods();
  ConcurrentMap<const VirtualScope*, std::unordered_set<const DexMethod*>>
      m_mergeable_scope_methods;

  void compute_mergeable_pairs_by_virtual_scopes(
      const method_profiles::MethodProfiles&);
  std::map<const VirtualScope*,
           std::vector<std::pair<const DexMethod*, const DexMethod*>>,
           virtualscopes_comparator>
      m_mergeable_pairs_by_virtual_scopes;

  void merge_methods();
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
  bool m_use_profiles{true};
};
