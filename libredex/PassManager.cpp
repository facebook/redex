/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PassManager.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <unordered_set>

#include "ApkManager.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "InstructionLowering.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "PrintSeeds.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Timer.h"
#include "Walkers.h"

namespace {

std::string get_apk_dir(const Json::Value& config) {
  auto apkdir = config["apk_dir"].asString();
  apkdir.erase(std::remove(apkdir.begin(), apkdir.end(), '"'), apkdir.end());
  return apkdir;
}

}

redex::ProguardConfiguration empty_pg_config() {
  redex::ProguardConfiguration pg_config;
  return pg_config;
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const Json::Value& config,
                         bool verify_none_mode)
    : PassManager(passes, empty_pg_config(), config, verify_none_mode) {
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const redex::ProguardConfiguration& pg_config,
                         const Json::Value& config,
                         bool verify_none_mode)
    : m_config(config),
      m_apk_mgr(get_apk_dir(config)),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(pg_config),
      m_testing_mode(false),
      m_verify_none_mode(verify_none_mode) {
  init(config);
  if (getenv("PROFILE_COMMAND") && getenv("PROFILE_PASS")) {
    std::string pass_name{getenv("PROFILE_PASS")};
    // Resolve the pass in the constructor so that any typos / references to
    // nonexistent passes are caught as early as possible
    auto pass_it = std::find_if(
        m_activated_passes.begin(),
        m_activated_passes.end(),
        [&pass_name](const Pass* pass) { return pass->name() == pass_name; });
    m_profiler_info = ProfilerInfo(getenv("PROFILE_COMMAND"), *pass_it);
    fprintf(stderr, "Will run profiler for %s\n", pass_name.c_str());
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
      fprintf(
          stderr, "ABORT! Inconsistency found in Dex code. %s\n", msg.c_str());
      fprintf(stderr, "Code:\n%s\n", SHOW(dex_method->get_code()));
      exit(EXIT_FAILURE);
    }
  });
}

const std::string PASS_ORDER_KEY = "pass_order";

/*
 * Appends the PID of the current process to :cmd and invokes it.
 */
pid_t spawn_profiler(const std::string& cmd) {
#ifdef _POSIX_VERSION
  auto parent = getpid();
  auto child = fork();
  if (child == -1) {
    always_assert_log(false, "Failed to fork");
    not_reached();
  } else if (child != 0) {
    return child;
  } else {
    std::ostringstream ss;
    ss << cmd << " " << parent;
    auto full_cmd = ss.str();
    execl("/bin/sh", "/bin/sh", "-c", full_cmd.c_str(), nullptr);
    always_assert_log(false, "exec of profiler command failed");
    not_reached();
  }
#else
  fprintf(stderr, "spawn_profiler() is a no-op");
  return 0;
#endif
}

pid_t kill_and_wait(pid_t pid, int sig) {
#ifdef _POSIX_VERSION
  kill(pid, sig);
  return waitpid(pid, nullptr, 0);
#else
  fprintf(stderr, "kill_and_wait() is a no-op");
  return 0;
#endif
}

void PassManager::run_passes(DexStoresVector& stores,
                             const Scope& external_classes,
                             ConfigFiles& cfg) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("Initializing reachable classes");
    init_reachable_classes(
        scope, m_config, m_pg_config, cfg.get_no_optimizations_annos());
  }
  {
    Timer t("Processing proguard rules");
    process_proguard_rules(
        cfg.get_proguard_map(), scope, external_classes, &m_pg_config);
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
    std::ofstream shrinking_file(cfg.get_printseeds() + ".allowshrinking");
    redex::print_seeds(
        shrinking_file, cfg.get_proguard_map(), scope, true, false);
    std::ofstream obfuscation_file(cfg.get_printseeds() + ".allowobfuscation");
    redex::print_seeds(
        obfuscation_file, cfg.get_proguard_map(), scope, false, true);
  }

  // Count the number of appearances of each pass name.
  const auto pass_repeats = [&]() {
    std::unordered_map<const Pass*, size_t> pass_repeats;
    for (const auto& pass : m_activated_passes) {
      ++pass_repeats[pass];
    }
    return pass_repeats;
  }();

  std::unordered_map<const Pass*, size_t> pass_counters;
  m_pass_info.resize(m_activated_passes.size());
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Evaluating %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    const size_t count = pass_counters[pass]++;
    m_pass_info[i].pass = pass;
    m_pass_info[i].order = i;
    m_pass_info[i].repeat = count;
    m_pass_info[i].total_repeat = pass_repeats.at(pass);
    m_pass_info[i].name = pass->name() + "#" + std::to_string(count + 1);
    m_pass_info[i].metrics[PASS_ORDER_KEY] = i;
    m_current_pass_info = &m_pass_info[i];
    pass->eval_pass(stores, cfg, *this);
    m_current_pass_info = nullptr;
  }

  // Retrieve the type checker's settings.
  auto type_checker_args = m_config["ir_type_checker"];
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
    bool run_profiler{m_profiler_info && m_profiler_info->pass == pass};
    pid_t profiler{-1};
    if (run_profiler) {
      fprintf(stderr, "Running profiler...\n");
      profiler = spawn_profiler(m_profiler_info->command);
    }
    pass->run_pass(stores, cfg, *this);
    if (run_profiler) {
      fprintf(stderr, "Waiting for profiler to finish...\n");
      kill_and_wait(profiler, SIGINT);
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
      pass->configure_pass(PassConfig(cfg[name_str]));
      return;
    }
  }
  always_assert_log(false, "No pass named %s!", name);
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
