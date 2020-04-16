/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PassManager.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <unordered_set>

#include "ApiLevelChecker.h"
#include "ApkManager.h"
#include "CommandProfiling.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "GraphVisualizer.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "InstructionLowering.h"
#include "JemallocUtil.h"
#include "OptData.h"
#include "PrintSeeds.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Sanitizers.h"
#include "Timer.h"
#include "Walkers.h"

namespace {

const std::string PASS_ORDER_KEY = "pass_order";

constexpr const char* CFG_DUMP_BASE_NAME = "redex-cfg-dumps.cfg";

std::string get_apk_dir(const Json::Value& config) {
  auto apkdir = config["apk_dir"].asString();
  apkdir.erase(std::remove(apkdir.begin(), apkdir.end(), '"'), apkdir.end());
  return apkdir;
}

// TODO(fengliu): Kill the `validate_access` flag.
void run_verifier(const Scope& scope,
                  bool verify_moves,
                  bool check_no_overwrite_this,
                  bool validate_access) {
  TRACE(PM, 1, "Running IRTypeChecker...");
  Timer t("IRTypeChecker");
  walk::parallel::methods(scope, [=](DexMethod* dex_method) {
    IRTypeChecker checker(dex_method, validate_access);
    if (verify_moves) {
      checker.verify_moves();
    }
    if (check_no_overwrite_this) {
      checker.check_no_overwrite_this();
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

} // namespace

std::unique_ptr<keep_rules::ProguardConfiguration> empty_pg_config() {
  return std::make_unique<keep_rules::ProguardConfiguration>();
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const Json::Value& config,
                         const RedexOptions& options)
    : PassManager(passes, empty_pg_config(), config, options) {}

PassManager::PassManager(
    const std::vector<Pass*>& passes,
    std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
    const Json::Value& config,
    const RedexOptions& options)
    : m_apk_mgr(get_apk_dir(config)),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(std::move(pg_config)),
      m_redex_options(options),
      m_testing_mode(false) {
  init(config);
  if (getenv("PROFILE_COMMAND") && getenv("PROFILE_PASS")) {
    // Resolve the pass in the constructor so that any typos / references to
    // nonexistent passes are caught as early as possible
    auto pass = find_pass(getenv("PROFILE_PASS"));
    always_assert(pass != nullptr);
    boost::optional<std::string> post_cmd = boost::none;
    if (getenv("PROFILE_POST_COMMAND")) {
      post_cmd = std::string(getenv("PROFILE_POST_COMMAND"));
    }
    m_profiler_info = ProfilerInfo(getenv("PROFILE_COMMAND"), post_cmd, pass);
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
    m_pass_info[i].config = JsonWrapper(config[pass->name()]);
  }
}

hashing::DexHash PassManager::run_hasher(const char* pass_name,
                                         const Scope& scope) {
  TRACE(PM, 2, "Running hasher...");
  Timer t("Hasher");
  hashing::DexScopeHasher hasher(scope);
  auto hash = hasher.run();
  if (pass_name) {
    // log metric value in a way that fits into JSON number value
    set_metric("~result~code~hash~",
               hash.code_hash & ((((size_t)1) << 52) - 1));
    set_metric("~result~registers~hash~",
               hash.registers_hash & ((((size_t)1) << 52) - 1));
    set_metric("~result~signature~hash~",
               hash.signature_hash & ((((size_t)1) << 52) - 1));
  }
  auto registers_hash_string = hashing::hash_to_string(hash.registers_hash);
  auto code_hash_string = hashing::hash_to_string(hash.code_hash);
  auto signature_hash_string = hashing::hash_to_string(hash.signature_hash);
  TRACE(PM, 3, "[scope hash] %s: registers#%s, code#%s, signature#%s",
        pass_name ? pass_name : "(initial)", registers_hash_string.c_str(),
        code_hash_string.c_str(), signature_hash_string.c_str());
  return hash;
}

void PassManager::run_passes(DexStoresVector& stores, ConfigFiles& conf) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);

  {
    Timer t("API Level Checker");
    api::LevelChecker::init(m_redex_options.min_sdk, scope);
  }

  char* seeds_output_file = std::getenv("REDEX_SEEDS_FILE");
  if (seeds_output_file) {
    std::string seed_filename = seeds_output_file;
    Timer t("Writing seeds file " + seed_filename);
    std::ofstream seeds_file(seed_filename);
    keep_rules::print_seeds(seeds_file, conf.get_proguard_map(), scope, false,
                            false);
  }
  if (!conf.get_printseeds().empty()) {
    Timer t("Writing seeds to file " + conf.get_printseeds());
    std::ofstream seeds_file(conf.get_printseeds());
    keep_rules::print_seeds(seeds_file, conf.get_proguard_map(), scope);
    std::ofstream config_file(conf.get_printseeds() + ".pro");
    keep_rules::show_configuration(config_file, scope, *m_pg_config);
    std::ofstream incoming(conf.get_printseeds() + ".incoming");
    redex::print_classes(incoming, conf.get_proguard_map(), scope);
    std::ofstream shrinking_file(conf.get_printseeds() + ".allowshrinking");
    keep_rules::print_seeds(shrinking_file, conf.get_proguard_map(), scope,
                            true, false);
    std::ofstream obfuscation_file(conf.get_printseeds() + ".allowobfuscation");
    keep_rules::print_seeds(obfuscation_file, conf.get_proguard_map(), scope,
                            false, true);
  }

  // Enable opt decision logging if specified in config.
  const Json::Value& opt_decisions_args =
      conf.get_json_config()["opt_decisions"];
  if (opt_decisions_args.get("enable_logs", false).asBool()) {
    opt_metadata::OptDataMapper::get_instance().enable_logs();
  }

  // Load configurations regarding the scope.
  conf.load(scope);

  sanitizers::lsan_do_recoverable_leak_check();

  // TODO(fengliu) : Remove Pass::eval_pass API
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Evaluating %s...", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    m_current_pass_info = &m_pass_info[i];
    pass->eval_pass(stores, conf, *this);
    m_current_pass_info = nullptr;
  }

  // Retrieve the hasher's settings.
  const Json::Value& hasher_args = conf.get_json_config()["hasher"];
  bool run_hasher_after_each_pass =
      hasher_args.get("run_after_each_pass", true).asBool();

  if (get_redex_options().disable_dex_hasher) {
    run_hasher_after_each_pass = false;
  }

  // Retrieve the type checker's settings.
  const Json::Value& type_checker_args =
      conf.get_json_config()["ir_type_checker"];
  bool run_type_checker_after_each_pass =
      type_checker_args.get("run_after_each_pass", false).asBool();
  bool verify_moves = type_checker_args.get("verify_moves", true).asBool();
  bool check_no_overwrite_this =
      type_checker_args.get("check_no_overwrite_this", false).asBool();
  std::unordered_set<std::string> type_checker_trigger_passes;

  for (auto& trigger_pass : type_checker_args["run_after_passes"]) {
    type_checker_trigger_passes.insert(trigger_pass.asString());
  }

  if (run_hasher_after_each_pass) {
    m_initial_hash =
        boost::optional<hashing::DexHash>(this->run_hasher(nullptr, scope));
  }

  // CFG visualizer infra. Dump all given classes.
  visualizer::Classes class_cfgs(
      conf.metafile(CFG_DUMP_BASE_NAME),
      conf.get_json_config().get("write_cfg_each_pass", false));
  class_cfgs.add_all(
      conf.get_json_config().get("dump_cfg_classes", std::string("")));
  constexpr visualizer::Options VISUALIZER_PASS_OPTIONS = (visualizer::Options)(
      visualizer::Options::SKIP_NO_CHANGE | visualizer::Options::FORCE_CFG);

  sanitizers::lsan_do_recoverable_leak_check();

  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Running %s...", pass->name().c_str());
    Timer t(pass->name() + " (run)");
    m_current_pass_info = &m_pass_info[i];

    {
      bool run_profiler = m_profiler_info && m_profiler_info->pass == pass;
      ScopedCommandProfiling cmd_prof(
          run_profiler ? boost::make_optional(m_profiler_info->command)
                       : boost::none,
          run_profiler ? m_profiler_info->post_cmd : boost::none);
      jemalloc_util::ScopedProfiling malloc_prof(m_malloc_profile_pass == pass);
      pass->run_pass(stores, conf, *this);
    }
    sanitizers::lsan_do_recoverable_leak_check();
    walk::parallel::code(build_class_scope(stores), [](DexMethod* m,
                                                       IRCode& code) {
      // Ensure that pass authors deconstructed the editable CFG at the end of
      // their pass. Currently, passes assume the incoming code will be in
      // IRCode form
      always_assert_log(!code.editable_cfg_built(), "%s has a cfg!", SHOW(m));
    });

    class_cfgs.add_pass(pass->name(), VISUALIZER_PASS_OPTIONS);

    bool run_hasher = run_hasher_after_each_pass;
    bool run_type_checker = run_type_checker_after_each_pass ||
                            type_checker_trigger_passes.count(pass->name()) > 0;

    if (run_hasher || run_type_checker) {
      scope = build_class_scope(it);
      if (run_hasher) {
        m_current_pass_info->hash = boost::optional<hashing::DexHash>(
            this->run_hasher(pass->name().c_str(), scope));
      }
      if (run_type_checker) {
        // It's OK to overwrite the `this` register if we are not yet at the
        // output phase -- the register allocator can fix it up later.
        run_verifier(scope, verify_moves,
                     /* check_no_overwrite_this */ false,
                     /* validate_access */ false);
      }
    }
    m_current_pass_info = nullptr;
  }

  // Always run the type checker before generating the optimized dex code.
  scope = build_class_scope(it);
  run_verifier(scope, verify_moves, get_redex_options().no_overwrite_this(),
               /* validate_access */ true);

  class_cfgs.add_pass("After all passes");
  class_cfgs.write();

  if (!conf.get_printseeds().empty()) {
    Timer t("Writing outgoing classes to file " + conf.get_printseeds() +
            ".outgoing");
    // Recompute the scope.
    scope = build_class_scope(it);
    std::ofstream outgoing(conf.get_printseeds() + ".outgoing");
    redex::print_classes(outgoing, conf.get_proguard_map(), scope);
  }

  sanitizers::lsan_do_recoverable_leak_check();
}

void PassManager::activate_pass(const char* name, const Json::Value& conf) {
  std::string name_str(name);

  // Names may or may not have a "#<id>" suffix to indicate their order in the
  // pass list, which needs to be removed for matching.
  std::string pass_name = name_str.substr(0, name_str.find('#'));
  for (auto pass : m_registered_passes) {
    if (pass_name == pass->name()) {
      m_activated_passes.push_back(pass);

      // Retrieving the configuration specific to this particular run
      // of the pass.
      pass->parse_config(JsonWrapper(conf[name_str]));
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
