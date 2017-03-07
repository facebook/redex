/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"
#include "ProguardConfiguration.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <json/json.h>

class PassManager {
 public:
  PassManager(
    const std::vector<Pass*>& passes,
    const Json::Value& config = Json::Value(Json::objectValue));
  struct PassMetrics {
    std::string name;
    std::unordered_map<std::string, int> metrics;
  };
  void run_passes(DexStoresVector&, ConfigFiles&);
  void incr_metric(const std::string& key, int value);
  void set_metric(const std::string& key, int value);
  int get_metric(const std::string& key);
  std::vector<PassManager::PassMetrics> get_metrics() const;
  const Json::Value& get_config() const { return m_config; }

  // A temporary hack to return the interdex metrics. Will be removed later.
  std::unordered_map<std::string, int> get_interdex_metrics();

  PassManager(
    const std::vector<Pass*>& passes,
    const redex::ProguardConfiguration& pg_config,
    const Json::Value& config = Json::Value(Json::objectValue));

  redex::ProguardConfiguration& get_proguard_config() { return m_pg_config; }
  bool no_proguard_rules() {
    return m_pg_config.keep_rules.empty() && !m_testing_mode;;
  }
  // Cal set_testing_mode() in tests that need passes to run which
  // do not use ProGuard configuratoion keep rules.
  void set_testing_mode() { m_testing_mode = true; }

 private:
  void activate_pass(const char* name, const Json::Value& cfg);

  Json::Value m_config;
  std::vector<Pass*> m_registered_passes;
  std::vector<Pass*> m_activated_passes;

  //per-pass metrics
  std::vector<PassManager::PassMetrics>  m_pass_metrics;
  std::unordered_map<std::string, int>* m_current_pass_metrics;

  // A temporary hack to remember where the inderdex matrics is store in the
  // m_pass_metrics vector. Will be removed later.
  int m_interdex_location = -1;

  redex::ProguardConfiguration m_pg_config;
  bool m_testing_mode{false};
};
