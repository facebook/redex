/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "InlinerConfig.h"
#include "RemoveBuilderPattern.h"
#include "Resolver.h"
#include "TypeSystem.h"

class MultiMethodInliner;

namespace builder_pattern {

using InstantiationToUsage =
    std::unordered_map<const IRInstruction*, std::vector<const IRInstruction*>>;

class BuilderTransform {
 public:
  BuilderTransform(const Scope& scope,
                   const TypeSystem& type_system,
                   const DexType* root,
                   const inliner::InlinerConfig& inliner_config,
                   DexStoresVector& stores);
  ~BuilderTransform();

  bool inline_super_calls_and_ctors(const DexType* type);

  std::unordered_set<const IRInstruction*> get_not_inlined_insns(
      DexMethod* caller, const std::unordered_set<IRInstruction*>& insns);

  void update_virtual_calls(
      const std::unordered_map<IRInstruction*, DexType*>& insn_to_type);

  void replace_fields(const InstantiationToUsage& usage, DexMethod* method);

  void cleanup();

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
