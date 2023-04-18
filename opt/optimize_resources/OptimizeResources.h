/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalConfig.h"
#include "Pass.h"
#include "Trace.h"
#include "androidfw/ResourceTypes.h"

namespace opt_res {

// Allows for declaring custom logic to mark certain resource IDs as reachable.
// Implement a subclass (which will be registered in super class constructor) to
// get a callback during OptimizeResourcesPass's configure / run pass.
//
// This is meant to work like the PassRegistry, where static initialization of a
// plugin subclass will insert itself into a registery (to be consumed by
// OptimizeResourcesPass).
class ReachableResourcesPlugin {
 public:
  explicit ReachableResourcesPlugin(const std::string& name);
  virtual void configure(ConfigFiles& conf) {}
  // Given the directory holding the unpacked input zip, and a list of resource
  // names to their corresponding ids, return a list of resource IDs that should
  // be considered reachable.
  virtual std::unordered_set<uint32_t> get_reachable_resources(
      const std::string& unpack_dir,
      const std::map<std::string, std::vector<uint32_t>>& name_to_ids) const {
    return {};
  }
  virtual ~ReachableResourcesPlugin() {}
  const std::string& get_name() const { return m_name; }

 private:
  const std::string m_name;
};

struct ReachableResourcesPluginRegistry {
  struct compare_plugins {
    bool operator()(const ReachableResourcesPlugin* a,
                    const ReachableResourcesPlugin* b) const {
      return strcmp(a->get_name().c_str(), b->get_name().c_str()) < 0;
    }
  };
  static ReachableResourcesPluginRegistry& get();
  void register_plugin(ReachableResourcesPlugin* plugin);
  void sort() {
    std::sort(m_registered_plugins.begin(),
              m_registered_plugins.end(),
              compare_plugins());
  }
  const std::vector<ReachableResourcesPlugin*>& get_plugins() const;

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  ReachableResourcesPluginRegistry() {}
  ReachableResourcesPluginRegistry(const ReachableResourcesPluginRegistry&) =
      delete;

  std::vector<ReachableResourcesPlugin*> m_registered_plugins;
};
} // namespace opt_res

/**
 * Finds resource entries that are unlikely to be accessed by the application
 * and removes them. This is meant to reduce the file size of resources.arsc
 * entries, delete any associated files with dead resource table entries, and
 * increase the likelihood of uncovering more dead bytecode (i.e. allow classes
 * referenced by .xml layouts that are dead to actually be removed - see
 * LayoutReachabilityPass for that).
 *
 * CONFIGURING:
 * Resource names that are accessed dynamically (i.e. though
 * android.content.res.Resources) should be specified as kept in the pass
 * configuration. For more involved logic that needs to happen to keep certain
 * resources, implement a ReachableResourcesPlugin subclass.
 *
 * It is also recommended to enable "finalize_resource_table" in the resources
 * global config, so that dead structures in the resource table (i.e. strings)
 * can be pruned once after all resource optimizations are done.
 *
 * ASSUMPTIONS:
 * Resource values referneced from AndroidManifest.xml will be kept, as well as
 * references from animation xml files (todo: document why for the latter).
 *
 * CONSTRAINTS:
 * This pass is quite old, and it expects to be run against fairly unoptimized
 * bytecode. As a result, correct operation requires careful pass ordering.
 *
 * This pass must be run before InstructionSequenceOutlinerPass and
 * ReduceArrayLiteralsPass.
 *
 * "assume_id_inlined" below, if false means that this
 * pass can compact resource table entries, and it MUST NOT be run after any
 * tool has inlined resource identifiers into const opcodes. When false, the
 * pass will use sget opcodes on R class fields to discover which resources are
 * used. False value means this pass must happen before FinalInlinePass /
 * FinalInlinePassV2. If set to true, this pass can run later in the pass order,
 * but it will not adjust any resource identifier values and instead leave holes
 * in the resources.arsc file (this results in meaningful cleanup to the
 * datastructures, i.e. removing string data and stuff but can leave the
 * resulting file with empty/redundant space making it take up more disk space
 * than ideal).
 */
class OptimizeResourcesPass : public Pass {
 public:
  OptimizeResourcesPass() : Pass("OptimizeResourcesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}},
            {NoSpuriousGetClassCalls, {.preserves = true}}};
  }

  explicit OptimizeResourcesPass(const std::string& name) : Pass(name) {}

  void bind_config() override {
    bind("delete_unused_files", false, m_delete_unused_files);
    bind("assume_reachable_prefixes", {}, m_assume_reachable_prefixes);
    bind("disallowed_types", {}, m_disallowed_types);
    bind("assume_id_inlined", false, m_assume_id_inlined);
    bind("check_string_for_name", false, m_check_string_for_name);
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  std::unique_ptr<Pass> clone(const std::string& new_name) const override {
    return std::make_unique<OptimizeResourcesPass>(new_name);
  }

  static void report_metric(TraceModule trace_module,
                            const std::string& metric_name,
                            int metric_value,
                            PassManager& mgr);

  static void remap_resource_classes(
      DexStoresVector& stores,
      const std::map<uint32_t, uint32_t>& old_to_remapped_ids);

  static void remap_resource_class_arrays(
      DexStoresVector& stores,
      const ResourceConfig& global_resources_config,
      const std::map<uint32_t, uint32_t>& old_to_remapped_ids);

  static void remap_resource_class_arrays(
      DexStoresVector& stores,
      const GlobalConfig& global_config,
      const std::map<uint32_t, uint32_t>& old_to_remapped_ids);

  static std::unordered_set<uint32_t> find_code_resource_references(
      DexStoresVector& stores,
      ConfigFiles& conf,
      PassManager& mgr,
      const std::map<std::string, std::vector<uint32_t>>& name_to_ids,
      bool check_string_for_name,
      bool assume_id_inlined);

 private:
  bool m_delete_unused_files;
  std::vector<std::string> m_assume_reachable_prefixes;
  std::unordered_set<std::string> m_disallowed_types;

 protected:
  bool m_assume_id_inlined;
  bool m_check_string_for_name;
};
