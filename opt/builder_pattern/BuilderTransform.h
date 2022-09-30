/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "BuilderAnalysis.h"
#include "DexClass.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "TypeSystem.h"

namespace builder_pattern {

class BuilderTransform {
 public:
  BuilderTransform(const Scope& scope,
                   const TypeSystem& type_system,
                   const DexType* root,
                   const init_classes::InitClassesWithSideEffects&
                       init_classes_with_side_effects,
                   const inliner::InlinerConfig& inliner_config,
                   DexStoresVector& stores);
  ~BuilderTransform();

  bool inline_super_calls_and_ctors(const DexType* type);

  /**
   * Try to inline the given calls (`insns`) in the caller, and return the set
   * of call instructions that were not abled to be inlined.
   */
  std::unordered_set<IRInstruction*> try_inline_calls(
      DexMethod* caller,
      const std::unordered_set<IRInstruction*>& insns,
      std::vector<IRInstruction*>* deleted_insns);

  void update_virtual_calls(
      const std::unordered_map<IRInstruction*, DexType*>& insn_to_type);

  void replace_fields(const InstantiationToUsage& usage, DexMethod* method);

  void cleanup();

  shrinker::Shrinker& get_shrinker() { return m_inliner->get_shrinker(); }

 private:
  const TypeSystem& m_type_system;
  const DexType* m_root;
  std::unique_ptr<MultiMethodInliner> m_inliner;
  inliner::InlinerConfig m_inliner_config;
  ConcurrentMethodRefCache m_concurrent_resolved_refs;

  // Used for tracking changes that we need to restore.
  std::unordered_map<DexMethod*, DexMethod*> m_method_copy;
};

} // namespace builder_pattern
