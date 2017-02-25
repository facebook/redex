/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PassManager.h"

#include <cstdio>

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "PrintSeeds.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Timer.h"
#include "Transform.h"

redex::ProguardConfiguration empty_pg_config() {
  redex::ProguardConfiguration pg_config;
  return pg_config;
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const Json::Value& config)
    : m_config(config),
      m_registered_passes(passes),
      m_current_pass_metrics(nullptr),
      m_pg_config(empty_pg_config()) {
  if (config["redex"].isMember("passes")) {
    auto passes = config["redex"]["passes"];
    for (auto& pass : passes) {
      activate_pass(pass.asString().c_str(), config);
    }
  } else {
    // If config isn't set up, run all registered passes.
    m_activated_passes = m_registered_passes;
  }
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const redex::ProguardConfiguration& pg_config,
                         const Json::Value& config)
    : m_config(config),
      m_registered_passes(passes),
      m_current_pass_metrics(nullptr),
      m_pg_config(pg_config) {
  if (config["redex"].isMember("passes")) {
    auto passes = config["redex"]["passes"];
    for (auto& pass : passes) {
      activate_pass(pass.asString().c_str(), config);
    }
  } else {
    // If config isn't set up, run all registered passes.
    m_activated_passes = m_registered_passes;
  }
}

void PassManager::run_passes(DexStoresVector& stores, ConfigFiles& cfg) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("Initializing reachable classes");
    init_reachable_classes(scope,
                           m_config,
                           m_pg_config,
                           cfg.get_no_optimizations_annos());
  }
  {
    Timer t("Processing proguard rules");
    process_proguard_rules(cfg.get_proguard_map(), &m_pg_config, scope);
  }
  char* seeds_output_file = std::getenv("REDEX_SEEDS_FILE");
  if (seeds_output_file) {
    std::string seed_filename = seeds_output_file;
    Timer t("Writing seeds file " + seed_filename);
    std::ofstream seeds_file(seed_filename);
    redex::print_seeds(seeds_file, cfg.get_proguard_map(), scope, false, false);
  }
  if (!cfg.get_printseeds().empty()) {
    Timer t("Writing seeds to file " + cfg.get_printseeds());
    std::ofstream seeds_file(cfg.get_printseeds());
    redex::print_seeds(seeds_file, cfg.get_proguard_map(), scope);
    std::ofstream config_file(cfg.get_printseeds() + ".pro");
    redex::show_configuration(config_file, scope, m_pg_config);
    std::ofstream incoming(cfg.get_printseeds() + ".incoming");
    redex::print_classes(incoming, cfg.get_proguard_map(), scope);
    std::ofstream shrinking_file(cfg.get_printseeds()  + ".allowshrinking");
    redex::print_seeds(shrinking_file, cfg.get_proguard_map(), scope, true, false);
    std::ofstream obfuscation_file(cfg.get_printseeds() + ".allowobfuscation");
    redex::print_seeds(obfuscation_file, cfg.get_proguard_map(), scope, false, true);
  }
  for (auto pass : m_activated_passes) {
    TRACE(PM, 1, "Evaluating %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    m_current_pass_metrics = &m_pass_metrics[pass->name()];
    pass->eval_pass(stores, cfg, *this);
    m_current_pass_metrics = nullptr;
  }
  for (auto pass : m_activated_passes) {
    TRACE(PM, 1, "Running %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (run)");
    m_current_pass_metrics = &m_pass_metrics[pass->name()];
    if (pass->assumes_sync()) {
      MethodTransform::sync_all();
    }
    pass->run_pass(stores, cfg, *this);
    m_current_pass_metrics = nullptr;
  }

  MethodTransform::sync_all();
  if (!cfg.get_printseeds().empty()) {
    Timer t("Writing outgoing classes to file " + cfg.get_printseeds() +
            ".outgoing");
    // Recompute the scope.
    scope = build_class_scope(it);
    std::ofstream outgoig(cfg.get_printseeds() + ".outgoing");
    redex::print_classes(outgoig, cfg.get_proguard_map(), scope);
    redex::alert_seeds(std::cerr, scope);
  }
}

void PassManager::activate_pass(const char* name, const Json::Value& cfg) {
  for (auto pass : m_registered_passes) {
    if (name == pass->name()) {
      m_activated_passes.push_back(pass);
      pass->configure_pass(PassConfig(cfg[pass->name()]));
      return;
    }
  }
  always_assert_log(false, "No pass named %s!", name);
}

void PassManager::incr_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_metrics != nullptr, "No current pass!");
  (*m_current_pass_metrics)[key] += value;
}

void PassManager::set_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_metrics != nullptr, "No current pass!");
  (*m_current_pass_metrics)[key] = value;
}

int PassManager::get_metric(const std::string& key) {
  return (*m_current_pass_metrics)[key];
}

std::map<std::string, std::map<std::string, int>> PassManager::get_metrics()
    const {
  return m_pass_metrics;
}
