/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <list>
#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AnalysisUsage.h"
#include "AssetManager.h"
#include "DexHasher.h"
#include "JsonWrapper.h"
#include "RedexOptions.h"
#include "RedexProperties.h"
#include "RedexPropertyCheckerRegistry.h"

struct ConfigFiles;
class DexStore;
class Pass;
struct PassManagerConfig;

namespace Json {
class Value;
} // namespace Json

namespace keep_rules {
struct ProguardConfiguration;
} // namespace keep_rules

// Must match DexStore.
using DexStoresVector = std::vector<DexStore>;

namespace redex_properties {
class Manager;
} // namespace redex_properties

struct ReserveRefsInfo;
using ReserveRefsInfoList = std::list<std::pair<std::string, ReserveRefsInfo>>;
using ReserveRefsInfoHandle = ReserveRefsInfoList::iterator;

class PassManager {
 public:
  explicit PassManager(const std::vector<Pass*>& passes);
  explicit PassManager(const std::vector<Pass*>& passes,
                       const ConfigFiles& config,
                       const RedexOptions& options = RedexOptions{});

  PassManager(const std::vector<Pass*>& passes,
              std::unique_ptr<keep_rules::ProguardConfiguration> pg_config);
  PassManager(const std::vector<Pass*>& passes,
              std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
              const ConfigFiles& config,
              const RedexOptions& options = RedexOptions{},
              redex_properties::Manager* properties_manager = nullptr);

  ~PassManager();

  struct PassInfo {
    const Pass* pass;
    size_t order; // zero-based
    size_t repeat; // zero-based
    size_t total_repeat;
    std::string name;
    std::unordered_map<std::string, int64_t> metrics;
    JsonWrapper config;
    boost::optional<hashing::DexHash> hash;
    redex_properties::PropertyInteractions property_interactions;
  };

  void run_passes(DexStoresVector&, ConfigFiles&);
  void incr_metric(const std::string& key, int64_t value);
  void set_metric(const std::string& key, int64_t value);
  int64_t get_metric(const std::string& key);
  const std::vector<PassManager::PassInfo>& get_pass_info() const;
  boost::optional<hashing::DexHash> get_initial_hash() const {
    return m_initial_hash;
  }
  const RedexOptions& get_redex_options() const { return m_redex_options; }

  // A temporary hack to return the interdex metrics. Will be removed later.
  const std::unordered_map<std::string, int64_t>& get_interdex_metrics();

  const keep_rules::ProguardConfiguration& get_proguard_config() {
    return *m_pg_config;
  }

  // Call set_testing_mode() in tests that need passes to run which
  // do not use ProGuard configuration keep rules.
  void set_testing_mode() { m_testing_mode = true; }

  const PassInfo* get_current_pass_info() const { return m_current_pass_info; }

  AssetManager& asset_manager() { return m_asset_mgr; }

  void record_running_regalloc() { m_regalloc_has_run = true; }
  bool regalloc_has_run() const { return m_regalloc_has_run; }

  void record_init_class_lowering() { m_init_class_lowering_has_run = true; }
  bool init_class_lowering_has_run() const {
    return m_init_class_lowering_has_run;
  }

  void record_materialize_nullchecks() {
    m_materialize_nullchecks_has_run = true;
  }
  bool materialize_nullchecks_has_run() const {
    return m_materialize_nullchecks_has_run;
  }

  void record_running_interdex() { m_interdex_has_run = true; }
  bool interdex_has_run() const { return m_interdex_has_run; }

  void record_unreliable_virtual_scopes() {
    m_unreliable_virtual_scopes = true;
  }
  bool unreliable_virtual_scopes() const { return m_unreliable_virtual_scopes; }

  template <typename PassType>
  PassType* get_preserved_analysis() const {
    auto pass =
        m_preserved_analysis_passes.find(get_analysis_id_by_pass<PassType>());
    if (pass != m_preserved_analysis_passes.end()) {
      return static_cast<PassType*>(pass->second);
    }
    return nullptr;
  }

  Pass* find_pass(const std::string& pass_name) const;

  struct ActivatedPasses {
    std::vector<std::pair<Pass*, std::string>> activated_passes;
    std::vector<std::unique_ptr<Pass>> cloned_passes;
  };
  static ActivatedPasses compute_activated_passes(
      std::vector<Pass*> m_registered_passes,
      const ConfigFiles& config,
      PassManagerConfig* pm_config_override = nullptr);

  // Reserves refs in every dex, effectively lowering the capacity of each dex.
  // This is applied uniformly (e.g., cannot be a per-dex value).
  // All reserved refs must eventually released.
  ReserveRefsInfoHandle reserve_refs(const std::string& name,
                                     const ReserveRefsInfo& info);

  void release_reserved_refs(ReserveRefsInfoHandle handle);

  ReserveRefsInfo get_reserved_refs() const;

 private:
  void init(const ConfigFiles& config);

  hashing::DexHash run_hasher(const char* name, const Scope& scope);

  void eval_passes(DexStoresVector&, ConfigFiles&);

  void init_property_interactions(ConfigFiles& conf);

  void check_unreleased_reserved_refs();

  AssetManager m_asset_mgr;
  std::vector<Pass*> m_registered_passes;
  std::vector<Pass*> m_activated_passes;
  std::unordered_map<AnalysisID, Pass*> m_preserved_analysis_passes;

  // Per-pass information and metrics
  std::vector<PassManager::PassInfo> m_pass_info;
  PassInfo* m_current_pass_info;

  std::unique_ptr<const keep_rules::ProguardConfiguration> m_pg_config;
  const RedexOptions m_redex_options;
  bool m_testing_mode{false};
  bool m_regalloc_has_run{false};
  bool m_init_class_lowering_has_run{false};
  bool m_materialize_nullchecks_has_run{false};
  bool m_interdex_has_run{false};
  bool m_unreliable_virtual_scopes{false};
  ReserveRefsInfoList m_reserved_ref_infos;
  Pass* m_malloc_profile_pass{nullptr};

  boost::optional<hashing::DexHash> m_initial_hash;

  std::vector<std::unique_ptr<Pass>> m_cloned_passes;

  // unique_ptr to avoid header include.
  struct InternalFields;
  std::unique_ptr<InternalFields> m_internal_fields;

  redex_properties::Manager* m_properties_manager{nullptr};
};
