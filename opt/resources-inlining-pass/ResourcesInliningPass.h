/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Resource Value Inlining pass: optimization pass, which analyzes code
 * and resource information to represent application logic in an equivalent but
 * with a more efficient execution. Uses static analysis techniques to
 * understand data flow into the Android SDK methods, and analyzes UI data
 * structures to find the resource values that are constant.
 */

#pragma once

#include "DexUtil.h"
#include "Pass.h"
#include "RedexResources.h"
#include "androidfw/ResourceTypes.h"

/* Struct to hold all the information about resource values that can be possibly
 * inlined
 */
struct InlinableOptimization {
  IRInstruction* insn;
  std::variant<resources::InlinableValue, std::string> inlinable;
};

using MethodTransformsMap =
    InsertOnlyConcurrentMap<DexMethod*, std::vector<InlinableOptimization>>;

class ResourcesInliningPass : public Pass {
 public:
  ResourcesInliningPass() : Pass("ResourcesInliningPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
        {RenameClass, Preserves},
    };
  }

  void bind_config() override {
    bind("resource_type_names", {}, m_resource_type_names);
    bind("resource_entry_names", {}, m_resource_entry_names);
  }

  // Runs the pass on the given Scope
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static std::unordered_map<uint32_t, resources::InlinableValue>
  filter_inlinable_resources(
      ResourceTableFile* res_table,
      const std::unordered_map<uint32_t, resources::InlinableValue>&
          inlinable_resources,
      const std::unordered_set<std::string>& resource_type_names,
      const std::unordered_set<std::string>& resource_entry_names);

  /* This method finds possible transformations of invoke_virtuals and move
   * instructions that are also inlinable and are one of the following supported
   * API calls: android.content.res.Resources.getBoolean(int)
   * android.content.res.Resources.getColor(int)
   * android.content.res.Resources.getInteger(int)
   * android.content.res.Resources.getString(int)
   */
  static MethodTransformsMap find_transformations(
      const Scope&,
      const std::unordered_map<uint32_t, resources::InlinableValue>&,
      const std::map<uint32_t, std::string>& id_to_name,
      const std::vector<std::string>& type_names,
      const boost::optional<std::string>& package_name);

  static void inline_resource_values_dex(
      DexMethod* method,
      const std::vector<InlinableOptimization>& insn_inlinable,
      PassManager& mgr);

  std::unordered_set<std::string> m_resource_type_names;
  std::unordered_set<std::string> m_resource_entry_names;
};
