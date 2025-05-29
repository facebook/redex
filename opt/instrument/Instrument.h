/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include "DeterministicContainers.h"
#include "Pass.h"
#include "PassManager.h"

class DexMethod;

namespace interdex {
class InterDexPassPlugin;
} // namespace interdex

namespace instrument {

enum class ProfileTypeFlags {
  NotSpecified = 0,
  MethodCallCount = 1,
  MethodCallOrder = 2,
  BlockCoverage = 4,
  BlockCount = 8,
  SimpleMethodTracing = 1 | 2,
  BasicBlockTracing = 1 | 2 | 4,
  BasicBlockHitCount = 1 | 2 | 4 | 8,
};

class InstrumentPass : public Pass {
 public:
  InstrumentPass();
  ~InstrumentPass();

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Requires},
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
        {RenameClass, Preserves},
    };
  }

  void bind_config() override;
  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Helper functions for both method and block instrumentations.
  //
  constexpr static const char* STATS_FIELD_NAME = "sMethodStats";
  constexpr static const char* HIT_STATS_FIELD_NAME = "sHitStats";

  static void patch_array_size(DexClass* analysis_cls,
                               const std::string_view array_name,
                               const int array_size);
  static void patch_static_field(DexClass* analysis_cls,
                                 const std::string_view field_name,
                                 const int new_number);
  static bool is_included(const DexMethod* method,
                          const UnorderedSet<std::string>& set);

  static UnorderedMap<int /*shard_num*/, DexFieldRef*> patch_sharded_arrays(
      DexClass* cls,
      const size_t num_shards,
      const std::map<int /*shard_num*/, std::string>& suggested_names = {});

  static std::pair<UnorderedMap<int /*shard_num*/, DexMethod*>,
                   UnorderedSet<std::string>>
  generate_sharded_analysis_methods(
      DexClass* cls,
      const std::string& template_method_full_name,
      const UnorderedMap<int /*shard_num*/, DexFieldRef*>& array_fields,
      const size_t num_shards);

  struct Options {
    std::string instrumentation_strategy;
    std::string analysis_class_name;
    std::string analysis_method_name;
    UnorderedSet<std::string> blocklist;
    UnorderedSet<std::string> allowlist;
    std::string blocklist_file_name;
    std::string metadata_file_name;
    int64_t num_stats_per_method;
    int64_t num_shards;
    bool only_cold_start_class;
    UnorderedMap<DexMethod*, DexMethod*> methods_replacement;
    std::vector<std::string> analysis_method_names;
    int64_t max_num_blocks;
    bool instrument_catches;
    bool instrument_blocks_without_source_block;
    bool instrument_only_root_store;
    bool inline_onBlockHit;
    bool inline_onNonLoopBlockHit;
    bool apply_CSE_CopyProp;
    std::optional<std::string> analysis_package_prefix;
  };

 private:
  Options m_options;
  std::unique_ptr<interdex::InterDexPassPlugin> m_plugin;
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
  std::optional<size_t> m_integrity_types;
};

} // namespace instrument
