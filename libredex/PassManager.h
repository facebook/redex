/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ApkManager.h"
#include "DexHasher.h"
#include "Pass.h"
#include "ProguardConfiguration.h"
#include "RedexOptions.h"

#include <boost/optional.hpp>
#include <json/json.h>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

class PassManager {
 public:
  explicit PassManager(
      const std::vector<Pass*>& passes,
      const Json::Value& config = Json::Value(Json::objectValue),
      const RedexOptions& options = RedexOptions{});

  PassManager(const std::vector<Pass*>& passes,
              std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
              const Json::Value& config = Json::Value(Json::objectValue),
              const RedexOptions& options = RedexOptions{});

  struct PassInfo {
    const Pass* pass;
    size_t order; // zero-based
    size_t repeat; // zero-based
    size_t total_repeat;
    std::string name;
    std::unordered_map<std::string, int64_t> metrics;
    JsonWrapper config;
    boost::optional<hashing::DexHash> hash;
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

  keep_rules::ProguardConfiguration& get_proguard_config() {
    return *m_pg_config;
  }

  bool no_proguard_rules() {
    return m_pg_config->keep_rules.empty() && !m_testing_mode;
  }

  // Call set_testing_mode() in tests that need passes to run which
  // do not use ProGuard configuration keep rules.
  void set_testing_mode() { m_testing_mode = true; }

  const PassInfo* get_current_pass_info() const { return m_current_pass_info; }

  ApkManager& apk_manager() { return m_apk_mgr; }

  void record_running_regalloc() { m_regalloc_has_run = true; }

  bool regalloc_has_run() { return m_regalloc_has_run; }

  template <typename PassType>
  PassType* get_preserved_analysis() const {
    auto pass = m_preserved_analysis_passes.find(typeid(PassType).name());
    if (pass != m_preserved_analysis_passes.end()) {
      return dynamic_cast<PassType*>(pass->second);
    }
    return nullptr;
  }

 private:
  void activate_pass(const std::string& name, const Json::Value& cfg);

  Pass* find_pass(const std::string& pass_name) const;

  void init(const Json::Value& config);

  hashing::DexHash run_hasher(const char* name, const Scope& scope);

  ApkManager m_apk_mgr;
  std::vector<Pass*> m_registered_passes;
  std::vector<Pass*> m_activated_passes;
  std::unordered_map<AnalysisID, Pass*> m_preserved_analysis_passes;

  // Per-pass information and metrics
  std::vector<PassManager::PassInfo> m_pass_info;
  PassInfo* m_current_pass_info;

  std::unique_ptr<keep_rules::ProguardConfiguration> m_pg_config;
  const RedexOptions m_redex_options;
  bool m_testing_mode{false};
  bool m_regalloc_has_run{false};

  struct ProfilerInfo {
    std::string command;
    boost::optional<std::string> shutdown_cmd;
    boost::optional<std::string> post_cmd;
    const Pass* pass;
    ProfilerInfo(const std::string& command,
                 const boost::optional<std::string>& shutdown_cmd,
                 const boost::optional<std::string>& post_cmd,
                 const Pass* pass)
        : command(command),
          shutdown_cmd(shutdown_cmd),
          post_cmd(post_cmd),
          pass(pass) {}
  };

  boost::optional<ProfilerInfo> m_profiler_info;
  Pass* m_malloc_profile_pass{nullptr};
  boost::optional<hashing::DexHash> m_initial_hash;
};
