/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PassManager.h"

#include <boost/filesystem.hpp>
#include <cinttypes>
#include <cstdio>
#include <typeinfo>
#include <unordered_set>

#include "AnalysisUsage.h"
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
#include "MethodProfiles.h"
#include "OptData.h"
#include "Pass.h"
#include "PrintSeeds.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Sanitizers.h"
#include "Show.h"
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

struct CheckerConfig {
  explicit CheckerConfig(const ConfigFiles& conf) {
    const Json::Value& type_checker_args =
        conf.get_json_config()["ir_type_checker"];
    run_type_checker_on_input =
        type_checker_args.get("run_on_input", true).asBool();
    run_type_checker_on_input_ignore_access =
        type_checker_args.get("run_on_input_ignore_access", false).asBool();
    run_type_checker_after_each_pass =
        type_checker_args.get("run_after_each_pass", true).asBool();
    verify_moves = type_checker_args.get("verify_moves", true).asBool();
    check_no_overwrite_this =
        type_checker_args.get("check_no_overwrite_this", false).asBool();
    check_num_of_refs =
        type_checker_args.get("check_num_of_refs", false).asBool();

    for (auto& trigger_pass : type_checker_args["run_after_passes"]) {
      type_checker_trigger_passes.insert(trigger_pass.asString());
    }
  }

  void on_input(const Scope& scope) {
    if (!run_type_checker_on_input) {
      std::cerr << "Note: input type checking is turned off!" << std::endl;
      return;
    }
    auto res = run_verifier(scope, verify_moves,
                            /* check_no_overwrite_this= */ false,
                            /* validate_access= */ true,
                            /* exit_on_fail= */ false);
    if (!res) {
      return; // No issues.
    }
    if (!run_type_checker_on_input_ignore_access) {
      std::string msg = *res;
      msg +=
          "\n If you are confident that this does not matter (e.g., because "
          "you are using MakePublicPass), turn off accessibility checking on "
          "input with `-J ir_type_checker.run_on_input_ignore_access=true`.\n "
          "You may turn off all input checking with `-J "
          "ir_type_checker.run_on_input=false`.";
      fail_error(msg);
    }

    res = run_verifier(scope, verify_moves,
                       /* check_no_overwrite_this= */ false,
                       /* validate_access= */ false,
                       /* exit_on_fail= */ false);
    if (!res) {
      std::cerr << "Warning: input has accessibility issues. Continuing."
                << std::endl;
      return; // "No" issues.
    }
    std::string msg = *res;
    msg +=
        "\n If you are confident that this does not matter, turn off input "
        "checking with `-J ir_type_checker.run_on_input=false`.";
    fail_error(msg);
  }

  bool run_after_pass(const Pass* pass) {
    return run_type_checker_after_each_pass ||
           type_checker_trigger_passes.count(pass->name()) > 0;
  }

  /**
   * Return activated_passes.size() if the checking is turned off.
   * Otherwize, return 0 or the index of the last InterDexPass.
   */
  size_t min_pass_idx_for_dex_ref_check(
      const std::vector<Pass*>& activated_passes) {
    if (!check_num_of_refs) {
      return activated_passes.size();
    }
    size_t idx = 0;
    for (size_t i = 0; i < activated_passes.size(); i++) {
      if (activated_passes[i]->name() == "InterDexPass") {
        idx = i;
      }
    }
    return idx;
  }

  static void ref_validation(const DexStoresVector& stores,
                             const std::string& pass_name) {
    Timer t("ref_validation");
    auto check_ref_num = [pass_name](const DexClasses& classes) {
      constexpr size_t limit = 65536;
      std::unordered_set<DexMethodRef*> total_method_refs;
      std::unordered_set<DexFieldRef*> total_field_refs;
      std::unordered_set<DexType*> total_type_refs;
      for (const auto cls : classes) {
        std::vector<DexMethodRef*> method_refs;
        std::vector<DexFieldRef*> field_refs;
        std::vector<DexType*> type_refs;
        cls->gather_methods(method_refs);
        cls->gather_fields(field_refs);
        cls->gather_types(type_refs);
        total_type_refs.insert(type_refs.begin(), type_refs.end());
        total_field_refs.insert(field_refs.begin(), field_refs.end());
        total_method_refs.insert(method_refs.begin(), method_refs.end());
      }
      always_assert_log(total_method_refs.size() <= limit,
                        "%s adds too many method refs", pass_name.c_str());
      always_assert_log(total_field_refs.size() <= limit,
                        "%s adds too many field refs", pass_name.c_str());
      always_assert_log(total_type_refs.size() <= limit,
                        "%s adds too many type refs", pass_name.c_str());
    };
    for (const auto& store : stores) {
      for (const auto& classes : store.get_dexen()) {
        check_ref_num(classes);
      }
    }
  }

  // TODO(fengliu): Kill the `validate_access` flag.
  static boost::optional<std::string> run_verifier(const Scope& scope,
                                                   bool verify_moves,
                                                   bool check_no_overwrite_this,
                                                   bool validate_access,
                                                   bool exit_on_fail = true) {
    TRACE(PM, 1, "Running IRTypeChecker...");
    Timer t("IRTypeChecker");
    std::atomic<size_t> errors{0};
    boost::optional<std::string> first_error_msg;
    walk::parallel::methods(scope, [&](DexMethod* dex_method) {
      IRTypeChecker checker(dex_method, validate_access);
      if (verify_moves) {
        checker.verify_moves();
      }
      if (check_no_overwrite_this) {
        checker.check_no_overwrite_this();
      }
      checker.run();
      if (checker.fail()) {
        bool first = errors.fetch_add(1) == 0;
        if (first) {
          std::ostringstream oss;
          oss << "Inconsistency found in Dex code for " << show(dex_method)
              << std::endl
              << " " << checker.what() << std::endl
              << "Code:" << std::endl
              << show(dex_method->get_code());
          first_error_msg = oss.str();
        }
      }
    });

    if (errors.load() > 0 && exit_on_fail) {
      redex_assert(first_error_msg);
      fail_error(*first_error_msg, errors.load());
    }

    return first_error_msg;
  }

  static void fail_error(const std::string& error_msg, size_t errors = 1) {
    std::cerr << error_msg << std::endl;
    if (errors > 1) {
      std::cerr << "(" << (errors - 1) << " more issues!)" << std::endl;
    }
    _exit(EXIT_FAILURE);
  }

  std::unordered_set<std::string> type_checker_trigger_passes;
  bool run_type_checker_on_input;
  bool run_type_checker_after_each_pass;
  bool run_type_checker_on_input_ignore_access;
  bool verify_moves;
  bool check_no_overwrite_this;
  bool check_num_of_refs;
};

struct ScopedVmHWM {
  explicit ScopedVmHWM(bool enabled, bool reset) : enabled(enabled) {
    if (enabled) {
      if (reset) {
        try_reset_hwm_mem_stat();
      }
      before = get_mem_stats().vm_hwm;
    }
  }

  void trace_log(PassManager* mgr, const Pass* pass) {
    if (enabled) {
      uint64_t after = get_mem_stats().vm_hwm;
      if (mgr != nullptr) {
        mgr->set_metric("vm_hwm_after", after);
        mgr->set_metric("vm_hwm_delta", after - before);
      }
      TRACE(STATS, 1, "VmHWM for %s was %s (%s over start).",
            pass->name().c_str(), pretty_bytes(after).c_str(),
            pretty_bytes(after - before).c_str());
    }
  }

  uint64_t before;
  bool enabled;
};

static void check_unique_deobfuscated_names(const char* pass_name,
                                            const Scope& scope) {
  TRACE(PM, 1, "Running check_unique_deobfuscated_names...");
  Timer t("check_unique_deobfuscated_names");
  std::unordered_map<std::string, DexMethod*> method_names;
  walk::methods(scope, [&method_names, pass_name](DexMethod* dex_method) {
    auto it = method_names.find(dex_method->get_deobfuscated_name());
    if (it != method_names.end()) {
      fprintf(stderr,
              "ABORT! [%s] Duplicate deobfuscated method name: %s\nfor %s\n vs "
              "%s\n",
              pass_name, it->first.c_str(), SHOW(dex_method), SHOW(it->second));
      exit(EXIT_FAILURE);
    }
    method_names.emplace(dex_method->get_deobfuscated_name(), dex_method);
  });
  std::unordered_map<std::string, DexField*> field_names;
  walk::fields(scope, [&field_names, pass_name](DexField* dex_field) {
    auto it = field_names.find(dex_field->get_deobfuscated_name());
    if (it != field_names.end()) {
      fprintf(
          stderr,
          "ABORT! [%s] Duplicate deobfuscated field name: %s\nfor %s\n vs %s\n",
          pass_name, it->first.c_str(), SHOW(dex_field), SHOW(it->second));
      exit(EXIT_FAILURE);
    }
    field_names.emplace(dex_field->get_deobfuscated_name(), dex_field);
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
    boost::optional<std::string> shutdown_cmd = boost::none;
    if (getenv("PROFILE_SHUTDOWN_COMMAND")) {
      shutdown_cmd = std::string(getenv("PROFILE_SHUTDOWN_COMMAND"));
    }
    boost::optional<std::string> post_cmd = boost::none;
    if (getenv("PROFILE_POST_COMMAND")) {
      post_cmd = std::string(getenv("PROFILE_POST_COMMAND"));
    }
    m_profiler_info =
        ProfilerInfo(getenv("PROFILE_COMMAND"), shutdown_cmd, post_cmd, pass);
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
    const auto& redex = config["redex"];
    auto passes_from_config = redex["passes"];
    for (const auto& pass : passes_from_config) {
      std::string pass_name = pass.asString();

      // Check whether it is explicitly disabled.
      auto is_disabled = [&config, &pass_name]() {
        if (!config.isMember(pass_name)) {
          return false;
        }
        const auto& pass_data = config[pass_name];
        if (!pass_data.isMember("disabled")) {
          return false;
        }
        return pass_data["disabled"].asBool();
      };
      if (is_disabled()) {
        continue;
      }

      activate_pass(pass_name, config);
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

  // Clear stale data. Make sure we start fresh.
  m_preserved_analysis_passes.clear();

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

  // Retrieve check_unique_deobfuscated_names settings.
  const Json::Value& check_unique_deobfuscated_names_args =
      conf.get_json_config()["check_unique_deobfuscated_names"];
  bool run_check_unique_deobfuscated_names_after_each_pass =
      check_unique_deobfuscated_names_args.get("run_after_each_pass", false)
          .asBool();
  bool run_check_unique_deobfuscated_names_initially =
      check_unique_deobfuscated_names_args.get("run_initially", false).asBool();
  bool run_check_unique_deobfuscated_names_finally =
      check_unique_deobfuscated_names_args.get("run_finally", false).asBool();

  // Retrieve the type checker's settings.
  CheckerConfig checker_conf{conf};
  checker_conf.on_input(scope);

  // Pull on method-profiles, so that they get initialized, and are matched
  // against the *initial* methods
  conf.get_method_profiles();

  if (run_hasher_after_each_pass) {
    m_initial_hash =
        boost::optional<hashing::DexHash>(this->run_hasher(nullptr, scope));
  }

  if (run_check_unique_deobfuscated_names_initially) {
    check_unique_deobfuscated_names("<initial>", scope);
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

  const bool hwm_pass_stats =
      traceEnabled(STATS, 1) || conf.get_json_config().get("mem_stats", true);
  const bool hwm_per_pass =
      conf.get_json_config().get("mem_stats_per_pass", true);

  size_t min_pass_idx_for_dex_ref_check =
      checker_conf.min_pass_idx_for_dex_ref_check(m_activated_passes);

  // Abort if the analysis pass dependencies are not satisfied.
  AnalysisUsage::check_dependencies(m_activated_passes);

  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    AnalysisUsage analysis_usage;
    pass->set_analysis_usage(analysis_usage);

    TRACE(PM, 1, "Running %s...", pass->name().c_str());
    ScopedVmHWM vm_hwm{hwm_pass_stats, hwm_per_pass};
    Timer t(pass->name() + " (run)");
    m_current_pass_info = &m_pass_info[i];

    {
      bool run_profiler = m_profiler_info && m_profiler_info->pass == pass;
      ScopedCommandProfiling cmd_prof(
          run_profiler ? boost::make_optional(m_profiler_info->command)
                       : boost::none,
          run_profiler ? m_profiler_info->shutdown_cmd : boost::none,
          run_profiler ? m_profiler_info->post_cmd : boost::none);
      jemalloc_util::ScopedProfiling malloc_prof(m_malloc_profile_pass == pass);
      pass->run_pass(stores, conf, *this);
    }

    vm_hwm.trace_log(this, pass);

    sanitizers::lsan_do_recoverable_leak_check();
    walk::parallel::code(build_class_scope(stores), [](DexMethod* m,
                                                       IRCode& code) {
      // Ensure that pass authors deconstructed the editable CFG at the end of
      // their pass. Currently, passes assume the incoming code will be in
      // IRCode form
      always_assert_log(!code.editable_cfg_built(), "%s has a cfg!", SHOW(m));
    });

    class_cfgs.add_pass(
        [&]() { return pass->name() + "(" + std::to_string(i) + ")"; },
        VISUALIZER_PASS_OPTIONS);

    bool run_hasher = run_hasher_after_each_pass;
    bool run_type_checker = checker_conf.run_after_pass(pass);

    if (run_hasher || run_type_checker ||
        run_check_unique_deobfuscated_names_after_each_pass) {
      scope = build_class_scope(it);
      if (run_hasher) {
        m_current_pass_info->hash = boost::optional<hashing::DexHash>(
            this->run_hasher(pass->name().c_str(), scope));
      }
      if (run_type_checker) {
        // It's OK to overwrite the `this` register if we are not yet at the
        // output phase -- the register allocator can fix it up later.
        CheckerConfig::run_verifier(scope, checker_conf.verify_moves,
                                    /* check_no_overwrite_this */ false,
                                    /* validate_access */ false);
      }
      if (i >= min_pass_idx_for_dex_ref_check) {
        CheckerConfig::ref_validation(stores, pass->name());
      }
      if (run_check_unique_deobfuscated_names_after_each_pass) {
        check_unique_deobfuscated_names(pass->name().c_str(), scope);
      }
    }

    // Invalidate existing preserved analyses according to policy set by each
    // pass.
    analysis_usage.do_pass_invalidation(&m_preserved_analysis_passes);

    if (pass->is_analysis_pass()) {
      // If the pass is an analysis pass, preserve it.
      m_preserved_analysis_passes.emplace(get_analysis_id_by_pass(pass), pass);
    }

    // New methods might have been introduced by this pass; process previously
    // unresolved methods to see if we can match them now (so that future passes
    // using method profiles benefit)
    conf.process_unresolved_method_profile_lines();
    set_metric("~result~MethodProfiles~", conf.get_method_profiles().size());
    set_metric("~result~MethodProfiles~unresolved~",
               conf.get_method_profiles().unresolved_size());

    m_current_pass_info = nullptr;
  }

  // Always run the type checker before generating the optimized dex code.
  scope = build_class_scope(it);
  CheckerConfig::run_verifier(scope, checker_conf.verify_moves,
                              get_redex_options().no_overwrite_this(),
                              /* validate_access */ true);

  if (run_check_unique_deobfuscated_names_finally) {
    check_unique_deobfuscated_names("<final>", scope);
  }

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

void PassManager::activate_pass(const std::string& name,
                                const Json::Value& conf) {
  // Names may or may not have a "#<id>" suffix to indicate their order in the
  // pass list, which needs to be removed for matching.
  std::string pass_name = name.substr(0, name.find('#'));
  for (auto pass : m_registered_passes) {
    if (pass_name == pass->name()) {
      m_activated_passes.push_back(pass);

      // Retrieving the configuration specific to this particular run
      // of the pass.
      pass->parse_config(JsonWrapper(conf[name]));
      return;
    }
  }
  not_reached_log("No pass named %s!", name.c_str());
}

Pass* PassManager::find_pass(const std::string& pass_name) const {
  auto pass_it = std::find_if(
      m_activated_passes.begin(),
      m_activated_passes.end(),
      [&pass_name](const Pass* pass) { return pass->name() == pass_name; });
  return pass_it != m_activated_passes.end() ? *pass_it : nullptr;
}

void PassManager::incr_metric(const std::string& key, int64_t value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] += value;
}

void PassManager::set_metric(const std::string& key, int64_t value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] = value;
}

int64_t PassManager::get_metric(const std::string& key) {
  return (m_current_pass_info->metrics)[key];
}

const std::vector<PassManager::PassInfo>& PassManager::get_pass_info() const {
  return m_pass_info;
}

const std::unordered_map<std::string, int64_t>&
PassManager::get_interdex_metrics() {
  for (const auto& pass_info : m_pass_info) {
    if (pass_info.pass->name() == "InterDexPass") {
      return pass_info.metrics;
    }
  }
  static std::unordered_map<std::string, int64_t> empty;
  return empty;
}
