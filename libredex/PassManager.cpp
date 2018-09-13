/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PassManager.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <unordered_set>

#include "ApkManager.h"
#include "CommandProfiling.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "InstructionLowering.h"
#include "JemallocUtil.h"
#include "OptData.h"
#include "PrintSeeds.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Timer.h"
#include "Walkers.h"

namespace {

const std::string PASS_ORDER_KEY = "pass_order";

std::string get_apk_dir(const Json::Value& config) {
  auto apkdir = config["apk_dir"].asString();
  apkdir.erase(std::remove(apkdir.begin(), apkdir.end(), '"'), apkdir.end());
  return apkdir;
}

} // namespace

redex::ProguardConfiguration empty_pg_config() {
  redex::ProguardConfiguration pg_config;
  return pg_config;
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const Json::Value& config,
                         bool verify_none_mode,
                         bool is_art_build)
    : PassManager(
          passes, empty_pg_config(), config, verify_none_mode, is_art_build) {}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const redex::ProguardConfiguration& pg_config,
                         const Json::Value& config,
                         bool verify_none_mode,
                         bool is_art_build)
    : m_apk_mgr(get_apk_dir(config)),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(pg_config),
      m_testing_mode(false),
      m_verify_none_mode(verify_none_mode),
      m_art_build(is_art_build) {
  init(config);
  if (getenv("PROFILE_COMMAND") && getenv("PROFILE_PASS")) {
    // Resolve the pass in the constructor so that any typos / references to
    // nonexistent passes are caught as early as possible
    auto pass = find_pass(getenv("PROFILE_PASS"));
    always_assert(pass != nullptr);
    m_profiler_info = ProfilerInfo(getenv("PROFILE_COMMAND"), pass);
    fprintf(stderr, "Will run profiler for %s\n", pass->name().c_str());
  }
  if (getenv("MALLOC_PROFILE_PASS")) {
    m_malloc_profile_pass = find_pass(getenv("MALLOC_PROFILE_PASS"));
    always_assert(m_malloc_profile_pass != nullptr);
    fprintf(stderr, "Will run jemalloc profiler for %s\n",
            m_malloc_profile_pass->name().c_str());
  }
}

void PassManager::init(const Json::Value& config) {
  if (config["redex"].isMember("passes")) {
    auto passes_from_config = config["redex"]["passes"];
    for (auto& pass : passes_from_config) {
      activate_pass(pass.asString().c_str(), config);
    }
  } else {
    // If config isn't set up, run all registered passes.
    m_activated_passes = m_registered_passes;
  }

  // Count the number of appearances of each pass name.
  std::unordered_map<const Pass*, size_t> pass_repeats;
  for (const Pass* pass : m_activated_passes) {
    ++pass_repeats[pass];
  }

  // Init m_pass_info
  std::unordered_map<const Pass*, size_t> pass_counters;
  m_pass_info.resize(m_activated_passes.size());
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    const size_t count = pass_counters[pass]++;
    m_pass_info[i].pass = pass;
    m_pass_info[i].order = i;
    m_pass_info[i].repeat = count;
    m_pass_info[i].total_repeat = pass_repeats.at(pass);
    m_pass_info[i].name = pass->name() + "#" + std::to_string(count + 1);
    m_pass_info[i].metrics[PASS_ORDER_KEY] = i;
  }
}

void PassManager::run_type_checker(const Scope& scope,
                                   bool polymorphic_constants,
                                   bool verify_moves) {
  TRACE(PM, 1, "Running IRTypeChecker...\n");
  Timer t("IRTypeChecker");
  walk::parallel::methods(scope, [=](DexMethod* dex_method) {
    IRTypeChecker checker(dex_method);
    if (polymorphic_constants) {
      checker.enable_polymorphic_constants();
    }
    if (verify_moves) {
      checker.verify_moves();
    }
    checker.run();
    if (checker.fail()) {
      std::string msg = checker.what();
      fprintf(stderr, "ABORT! Inconsistency found in Dex code for %s.\n %s\n",
              SHOW(dex_method), msg.c_str());
      fprintf(stderr, "Code:\n%s\n", SHOW(dex_method->get_code()));
      exit(EXIT_FAILURE);
    }
  });
}

void PassManager::run_passes(DexStoresVector& stores, ConfigFiles& cfg) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);

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
    std::ofstream shrinking_file(cfg.get_printseeds() + ".allowshrinking");
    redex::print_seeds(shrinking_file, cfg.get_proguard_map(), scope, true,
                       false);
    std::ofstream obfuscation_file(cfg.get_printseeds() + ".allowobfuscation");
    redex::print_seeds(obfuscation_file, cfg.get_proguard_map(), scope, false,
                       true);
  }

  // Enable opt decision logging if specified in config.
  const Json::Value& opt_decisions_args =
      cfg.get_json_config()["opt_decisions"];
  if (opt_decisions_args.get("enable_logs", false).asBool()) {
    opt_metadata::OptDataMapper::get_instance().enable_logs();
  }

  // TODO(fengliu) : Remove Pass::eval_pass API
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Evaluating %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    m_current_pass_info = &m_pass_info[i];
    pass->eval_pass(stores, cfg, *this);
    m_current_pass_info = nullptr;
  }

  // Retrieve the type checker's settings.
  const Json::Value& type_checker_args =
      cfg.get_json_config()["ir_type_checker"];
  bool run_after_each_pass =
      type_checker_args.get("run_after_each_pass", false).asBool();
  // When verify_none is enabled, it's OK to have polymorphic constants.
  bool polymorphic_constants =
      type_checker_args.get("polymorphic_constants", false).asBool() ||
      verify_none_enabled();
  bool verify_moves = type_checker_args.get("verify_moves", false).asBool();
  std::unordered_set<std::string> trigger_passes;

  for (auto& trigger_pass : type_checker_args["run_after_passes"]) {
    trigger_passes.insert(trigger_pass.asString());
  }

  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Running %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (run)");
    m_current_pass_info = &m_pass_info[i];

    {
      ScopedCommandProfiling cmd_prof(
          m_profiler_info && m_profiler_info->pass == pass
              ? boost::make_optional(m_profiler_info->command)
              : boost::none);
      jemalloc_util::ScopedProfiling malloc_prof(m_malloc_profile_pass == pass);
      pass->run_pass(stores, cfg, *this);
    }

    if (run_after_each_pass || trigger_passes.count(pass->name()) > 0) {
      scope = build_class_scope(it);
      run_type_checker(scope, polymorphic_constants, verify_moves);
    }
    m_current_pass_info = nullptr;
  }

  // Always run the type checker before generating the optimized dex code.
  scope = build_class_scope(it);
  run_type_checker(scope, polymorphic_constants, verify_moves);

  if (!cfg.get_printseeds().empty()) {
    Timer t("Writing outgoing classes to file " + cfg.get_printseeds() +
            ".outgoing");
    // Recompute the scope.
    scope = build_class_scope(it);
    std::ofstream outgoing(cfg.get_printseeds() + ".outgoing");
    redex::print_classes(outgoing, cfg.get_proguard_map(), scope);
  }
}

void PassManager::activate_pass(const char* name, const Json::Value& cfg) {
  std::string name_str(name);

  // Names may or may not have a "#<id>" suffix to indicate their order in the
  // pass list, which needs to be removed for matching.
  std::string pass_name = name_str.substr(0, name_str.find("#"));
  for (auto pass : m_registered_passes) {
    if (pass_name == pass->name()) {
      m_activated_passes.push_back(pass);

      // Retrieving the configuration specific to this particular run
      // of the pass.
      pass->configure_pass(JsonWrapper(cfg[name_str]));
      return;
    }
  }
  always_assert_log(false, "No pass named %s!", name);
}

Pass* PassManager::find_pass(const std::string& pass_name) const {
  auto pass_it = std::find_if(
      m_activated_passes.begin(),
      m_activated_passes.end(),
      [&pass_name](const Pass* pass) { return pass->name() == pass_name; });
  return pass_it != m_activated_passes.end() ? *pass_it : nullptr;
}

void PassManager::incr_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] += value;
}

void PassManager::set_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] = value;
}

int PassManager::get_metric(const std::string& key) {
  return (m_current_pass_info->metrics)[key];
}

const std::vector<PassManager::PassInfo>& PassManager::get_pass_info() const {
  return m_pass_info;
}

const std::unordered_map<std::string, int>&
PassManager::get_interdex_metrics() {
  for (const auto& pass_info : m_pass_info) {
    if (pass_info.pass->name() == "InterDexPass") {
      return pass_info.metrics;
    }
  }
  static std::unordered_map<std::string, int> empty;
  return empty;
}
