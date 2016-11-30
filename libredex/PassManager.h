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
#include <vector>
#include <json/json.h>

class PassManager {
 public:
  PassManager(
    const std::vector<Pass*>& passes,
    const Json::Value& config = Json::Value(Json::objectValue));
  void run_passes(DexStoresVector&, ConfigFiles&);
  void incr_metric(const std::string& key, int value);
  int get_metric(const std::string& key);
  std::map<std::string, std::map<std::string, int> > get_metrics() const;
  const Json::Value& get_config() const { return m_config; }

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
  std::map<std::string, std::map<std::string, int> > m_pass_metrics;
  std::map<std::string, int>* m_current_pass_metrics;

  redex::ProguardConfiguration m_pg_config;
  bool m_testing_mode{false};
};
