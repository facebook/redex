/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"
#include "Pass.h"
#include "RedexResources.h"
#include "androidfw/ResourceTypes.h"

struct InlinableOptimization {
  IRInstruction* insn;
  resources::InlinableValue inlinable_value;
};

using MethodTransformsMap =
    InsertOnlyConcurrentMap<DexMethod*, std::vector<InlinableOptimization>>;

class ResourcesInliningPass : public Pass {
 public:
  ResourcesInliningPass() : Pass("ResourcesInliningPass") {}

  void bind_config() override {
    bind("resource_type_names", {}, m_resource_type_names);
    bind("resource_entry_names", {}, m_resource_entry_names);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static MethodTransformsMap optimization_pass(
      const Scope&,
      const std::unordered_map<uint32_t, resources::InlinableValue>&);

  std::unordered_set<std::string> m_resource_type_names;
  std::unordered_set<std::string> m_resource_entry_names;
};
