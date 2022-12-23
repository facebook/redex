/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PassManager.h"
#include "DexAssessments.h"

#include <boost/filesystem.hpp>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <typeinfo>
#include <unordered_set>

#ifdef __linux__
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "AnalysisUsage.h"
#include "ApiLevelChecker.h"
#include "AssetManager.h"
#include "CFGMutation.h"
#include "CommandProfiling.h"
#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "GraphVisualizer.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "InstructionLowering.h"
#include "JemallocUtil.h"
#include "MethodProfiles.h"
#include "Native.h"
#include "OptData.h"
#include "Pass.h"
#include "PrintSeeds.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "Purity.h"
#include "ReachableClasses.h"
#include "Sanitizers.h"
#include "ScopedCFG.h"
#include "ScopedMemStats.h"
#include "ScopedMetrics.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Timer.h"
#include "Walkers.h"

namespace {

constexpr const char* INCOMING_HASHES = "incoming_hashes.txt";
constexpr const char* OUTGOING_HASHES = "outgoing_hashes.txt";
constexpr const char* REMOVABLE_NATIVES = "redex-removable-natives.txt";
const std::string PASS_ORDER_KEY = "pass_order";

const Pass* get_profiled_pass(const PassManager& mgr) {
  redex_assert(getenv("PROFILE_PASS") != nullptr);
  // Resolve the pass in the constructor so that any typos / references to
  // nonexistent passes are caught as early as possible
  auto pass = mgr.find_pass(getenv("PROFILE_PASS"));
  always_assert(pass != nullptr);
  std::cerr << "Will run profiler for " << pass->name() << std::endl;
  return pass;
}

std::string get_apk_dir(const ConfigFiles& config) {
  auto apkdir = config.get_json_config()["apk_dir"].asString();
  apkdir.erase(std::remove(apkdir.begin(), apkdir.end(), '"'), apkdir.end());
  return apkdir;
}

class CheckerConfig {
 public:
  explicit CheckerConfig(const ConfigFiles& conf) {
    const Json::Value& type_checker_args =
        conf.get_json_config()["ir_type_checker"];
    m_run_type_checker_on_input =
        type_checker_args.get("run_on_input", true).asBool();
    m_run_type_checker_on_input_ignore_access =
        type_checker_args.get("run_on_input_ignore_access", false).asBool();
    m_run_type_checker_after_each_pass =
        type_checker_args.get("run_after_each_pass", true).asBool();
    m_verify_moves = type_checker_args.get("verify_moves", true).asBool();
    m_validate_invoke_super =
        type_checker_args.get("validate_invoke_super", true).asBool();
    m_check_no_overwrite_this =
        type_checker_args.get("check_no_overwrite_this", false).asBool();

    m_annotated_cfg_on_error =
        type_checker_args.get("annotated_cfg_on_error", false).asBool();
    m_annotated_cfg_on_error_reduced =
        type_checker_args.get("annotated_cfg_on_error_reduced", true).asBool();

    m_check_num_of_refs =
        type_checker_args.get("check_num_of_refs", false).asBool();

    for (auto& trigger_pass : type_checker_args["run_after_passes"]) {
      m_type_checker_trigger_passes.insert(trigger_pass.asString());
    }
  }

  void on_input(const Scope& scope) {
    if (!m_run_type_checker_on_input) {
      std::cerr << "Note: input type checking is turned off!" << std::endl;
      return;
    }

    auto res =
        check_no_overwrite_this(false).validate_access(true).run_verifier(
            scope, /* exit_on_fail= */ false);
    if (!res) {
      return; // No issues.
    }
    if (!m_run_type_checker_on_input_ignore_access) {
      std::string msg = *res;
      msg +=
          "\n If you are confident that this does not matter (e.g., because "
          "you are using MakePublicPass), turn off accessibility checking on "
          "input with `-J ir_type_checker.run_on_input_ignore_access=true`.\n "
          "You may turn off all input checking with `-J "
          "ir_type_checker.run_on_input=false`.";
      fail_error(msg);
    }

    res = check_no_overwrite_this(false).validate_access(false).run_verifier(
        scope, /* exit_on_fail= */ false);
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
    return m_run_type_checker_after_each_pass ||
           m_type_checker_trigger_passes.count(pass->name()) > 0;
  }

  /**
   * Return activated_passes.size() if the checking is turned off.
   * Otherwize, return 0 or the index of the last InterDexPass.
   */
  size_t min_pass_idx_for_dex_ref_check(
      const std::vector<Pass*>& activated_passes) {
    if (!m_check_num_of_refs) {
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
    auto check_ref_num = [pass_name](const DexClasses& classes,
                                     const DexStore& store, size_t dex_id) {
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
      TRACE(PM, 1, "dex %s: method refs %zu, filed refs %zu, type refs %zu",
            dex_name(store, dex_id).c_str(), total_method_refs.size(),
            total_field_refs.size(), total_type_refs.size());
      always_assert_log(total_method_refs.size() <= limit,
                        "%s adds too many method refs", pass_name.c_str());
      always_assert_log(total_field_refs.size() <= limit,
                        "%s adds too many field refs", pass_name.c_str());
      always_assert_log(total_type_refs.size() <= limit,
                        "%s adds too many type refs", pass_name.c_str());
    };
    for (const auto& store : stores) {
      size_t dex_id = 0;
      for (const auto& classes : store.get_dexen()) {
        check_ref_num(classes, store, dex_id++);
      }
    }
  }

  // Literate style.
  CheckerConfig check_no_overwrite_this(bool val) const {
    CheckerConfig ret = *this;
    ret.m_check_no_overwrite_this = val;
    return ret;
  }
  CheckerConfig validate_access(bool val) const {
    CheckerConfig ret = *this;
    ret.m_validate_access = val;
    return ret;
  }

  boost::optional<std::string> run_verifier(const Scope& scope,
                                            bool exit_on_fail = true) {
    TRACE(PM, 1, "Running IRTypeChecker...");
    Timer t("IRTypeChecker");

    struct Result {
      size_t errors{0};
      DexMethod* smallest_error_method{nullptr};
      size_t smallest_size{std::numeric_limits<size_t>::max()};

      Result() = default;
      explicit Result(DexMethod* m)
          : errors(1),
            smallest_error_method(m),
            smallest_size(m->get_code()->count_opcodes()) {}

      Result& operator+=(const Result& other) {
        errors += other.errors;
        if (smallest_size > other.smallest_size) {
          smallest_size = other.smallest_size;
          smallest_error_method = other.smallest_error_method;
        }
        return *this;
      }
    };

    auto run_checker_tmpl = [&](DexMethod* dex_method, auto fn) {
      IRTypeChecker checker(dex_method, m_validate_access,
                            m_validate_invoke_super);
      if (m_verify_moves) {
        checker.verify_moves();
      }
      if (m_check_no_overwrite_this) {
        checker.check_no_overwrite_this();
      }
      return fn(std::move(checker));
    };
    auto run_checker = [&](DexMethod* dex_method) {
      return run_checker_tmpl(dex_method, [](auto checker) {
        checker.run();
        return checker;
      });
    };
    auto run_checker_error = [&](DexMethod* dex_method) {
      if (m_annotated_cfg_on_error) {
        return run_checker_tmpl(dex_method, [&](auto checker) {
          if (m_annotated_cfg_on_error_reduced) {
            return checker.dump_annotated_cfg_reduced(dex_method);
          } else {
            return checker.dump_annotated_cfg(dex_method);
          }
        });
      }
      return show(dex_method->get_code());
    };

    auto res =
        walk::parallel::methods<Result>(scope, [&](DexMethod* dex_method) {
          auto checker = run_checker(dex_method);
          if (!checker.fail()) {
            return Result();
          }
          return Result(dex_method);
        });

    if (res.errors == 0) {
      return boost::none;
    }

    // Re-run the smallest method to produce error message.
    auto checker = run_checker(res.smallest_error_method);
    redex_assert(checker.fail());

    std::ostringstream oss;
    oss << "Inconsistency found in Dex code for "
        << show(res.smallest_error_method) << std::endl
        << " " << checker.what() << std::endl
        << "Code:" << std::endl
        << run_checker_error(res.smallest_error_method);

    if (res.errors > 1) {
      oss << "\n(" << (res.errors - 1) << " more issues!)";
    }

    always_assert_log(!exit_on_fail, "%s", oss.str().c_str());
    return oss.str();
  }

  static void fail_error(const std::string& error_msg, size_t errors = 1) {
    std::cerr << error_msg << std::endl;
    if (errors > 1) {
      std::cerr << "(" << (errors - 1) << " more issues!)" << std::endl;
    }
    _exit(EXIT_FAILURE);
  }

 private:
  std::unordered_set<std::string> m_type_checker_trigger_passes;
  bool m_run_type_checker_on_input;
  bool m_run_type_checker_after_each_pass;
  bool m_run_type_checker_on_input_ignore_access;
  bool m_verify_moves;
  bool m_validate_invoke_super;
  bool m_check_no_overwrite_this;
  bool m_check_num_of_refs;
  // TODO(fengliu): Kill the `validate_access` flag.
  bool m_validate_access{true};
  bool m_annotated_cfg_on_error{false};
  bool m_annotated_cfg_on_error_reduced{true};
};

class CheckUniqueDeobfuscatedNames {
 public:
  bool m_after_each_pass{false};

  explicit CheckUniqueDeobfuscatedNames(ConfigFiles& conf) {
    const Json::Value& args =
        conf.get_json_config()["check_unique_deobfuscated_names"];
    m_after_each_pass = args.get("run_after_each_pass", false).asBool();
    m_initially = args.get("run_initially", false).asBool();
    m_finally = args.get("run_finally", false).asBool();
  }

  void run_initially(const Scope& scope) {
    if (m_initially) {
      check_unique_deobfuscated_names("<initial>", scope);
    }
  }

  void run_finally(const Scope& scope) {
    if (m_finally) {
      check_unique_deobfuscated_names("<final>", scope);
    }
  }

  void run_after_pass(const Pass* pass, const Scope& scope) {
    if (m_after_each_pass) {
      check_unique_deobfuscated_names(pass->name().c_str(), scope);
    }
  }

 private:
  void check_unique_deobfuscated_names(const char* pass_name,
                                       const Scope& scope) {
    TRACE(PM, 1, "Running check_unique_deobfuscated_names...");
    Timer t("check_unique_deobfuscated_names");
    std::unordered_map<const DexString*, DexMethod*> method_names;
    walk::methods(scope, [&method_names, pass_name](DexMethod* dex_method) {
      auto deob = dex_method->get_deobfuscated_name_or_null();
      auto it = method_names.find(deob);
      if (it != method_names.end()) {
        fprintf(
            stderr,
            "ABORT! [%s] Duplicate deobfuscated method name: %s\nfor %s\n vs "
            "%s\n",
            pass_name, it->first->c_str(), SHOW(dex_method), SHOW(it->second));
        exit(EXIT_FAILURE);
      }
      method_names.emplace(deob, dex_method);
    });
    std::unordered_map<std::string, DexField*> field_names;
    walk::fields(scope, [&field_names, pass_name](DexField* dex_field) {
      auto deob = dex_field->get_deobfuscated_name();
      auto it = field_names.find(deob);
      if (it != field_names.end()) {
        fprintf(stderr,
                "ABORT! [%s] Duplicate deobfuscated field name: %s\nfor %s\n "
                "vs %s\n",
                pass_name, it->first.c_str(), SHOW(dex_field),
                SHOW(it->second));
        exit(EXIT_FAILURE);
      }
      field_names.emplace(deob, dex_field);
    });
  }

  bool m_initially{false};
  bool m_finally{false};
};

class VisualizerHelper {
 public:
  explicit VisualizerHelper(const ConfigFiles& conf)
      : m_class_cfgs(conf.metafile(CFG_DUMP_BASE_NAME),
                     conf.get_json_config().get("write_cfg_each_pass", false)) {
    m_class_cfgs.add_all(
        conf.get_json_config().get("dump_cfg_classes", std::string("")));
  }

  void add_pass(const Pass* pass, size_t i) {
    m_class_cfgs.add_pass(
        [&]() { return pass->name() + "(" + std::to_string(i) + ")"; },
        VISUALIZER_PASS_OPTIONS);
  }

  void finalize() {
    m_class_cfgs.add_pass("After all passes");
    m_class_cfgs.write();
  }

 private:
  static constexpr visualizer::Options VISUALIZER_PASS_OPTIONS =
      (visualizer::Options)(visualizer::Options::SKIP_NO_CHANGE |
                            visualizer::Options::FORCE_CFG);
  static constexpr const char* CFG_DUMP_BASE_NAME = "redex-cfg-dumps.cfg";

  visualizer::Classes m_class_cfgs;
};

class AnalysisUsageHelper {
 public:
  using PreservedMap = std::unordered_map<AnalysisID, Pass*>;

  explicit AnalysisUsageHelper(PreservedMap& m)
      : m_preserved_analysis_passes(m) {}

  void pre_pass(Pass* pass) { pass->set_analysis_usage(m_analysis_usage); }

  void post_pass(Pass* pass) {
    // Invalidate existing preserved analyses according to policy set by each
    // pass.
    m_analysis_usage.do_pass_invalidation(&m_preserved_analysis_passes);

    if (pass->is_analysis_pass()) {
      // If the pass is an analysis pass, preserve it.
      m_preserved_analysis_passes.emplace(get_analysis_id_by_pass(pass), pass);
    }
  }

 private:
  AnalysisUsage m_analysis_usage;
  PreservedMap& m_preserved_analysis_passes;
};

class JNINativeContextHelper {
 public:
  explicit JNINativeContextHelper(const Scope& scope,
                                  const std::string& jni_output_dir) {
    if (!jni_output_dir.empty()) {
      // Currently, if the path is not found, the native context is going to be
      // empty.
      g_native_context = std::make_unique<native::NativeContext>(
          native::NativeContext::build(jni_output_dir, scope));

      // Before running any passes, treat everything as removable.
      walk::methods(scope, [this](DexMethod* m) {
        if (is_native(m)) {
          auto native_func = native::get_native_function_for_dex_method(m);
          if (native_func) {
            TRACE(NATIVE, 2, "Found native function %s",
                  native_func->get_name().c_str());
            m_removable_natives.emplace(native_func);
          } else {
            // There's a native method which we don't find. Let's be
            // conservative and ask Redex not to remove it.
            m->rstate.set_root();
            // Ignore "linking" failures for pre-existing "linking" failures.
            m_java_method_no_impl_on_input.emplace(m);
          }
        }
      });
    }
  }

  void post_passes(const Scope& scope, ConfigFiles& conf) {
    if (!g_native_context) {
      return;
    }
    // After running all passes, walk through the removable functions and
    // remove the ones should remain.
    walk::methods(scope, [this](DexMethod* m) {
      if (is_native(m)) {
        auto native_func = native::get_native_function_for_dex_method(m);
        if (native_func) {
          auto it = m_removable_natives.find(native_func);
          if (it != m_removable_natives.end()) {
            TRACE(NATIVE, 2, "Cannot remove native function %s, called as %s",
                  native_func->get_name().c_str(), SHOW(m));
            m_removable_natives.erase(it);
          }
        } else if (!m_java_method_no_impl_on_input.count(m)) {
          // TODO: "Linking" error: Change this to an assertion failure
          TRACE(PM, 1, "Unable to find native implementation for %s.", SHOW(m));
        }
      }
    });

    TRACE(NATIVE, 2, "Total removable natives: %lu",
          m_removable_natives.size());

    auto removable_natives_file_name = conf.metafile(REMOVABLE_NATIVES);
    std::vector<std::string> output_symbols;
    output_symbols.reserve(m_removable_natives.size());

    // Might be non-deterministic in order, put them in a vector and sort.
    for (auto func : m_removable_natives) {
      output_symbols.push_back(func->get_name());
    }

    std::sort(output_symbols.begin(), output_symbols.end());

    std::ofstream out(removable_natives_file_name);

    // TODO: For better human readability, change this to CSV of native,java?
    for (const auto& name : output_symbols) {
      out << name << std::endl;
    }

    g_native_context.reset();
  }

 private:
  std::unordered_set<native::Function*> m_removable_natives;
  std::unordered_set<DexMethod*> m_java_method_no_impl_on_input;
};

void process_method_profiles(PassManager& mgr, ConfigFiles& conf) {
  // New methods might have been introduced by this pass; process previously
  // unresolved methods to see if we can match them now (so that future passes
  // using method profiles benefit)
  conf.process_unresolved_method_profile_lines();
  mgr.set_metric("~result~MethodProfiles~", conf.get_method_profiles().size());
  mgr.set_metric("~result~MethodProfiles~unresolved~",
                 conf.get_method_profiles().unresolved_size());
}

void process_secondary_method_profiles(PassManager& mgr, ConfigFiles& conf) {
  // New methods might have been introduced by this pass; process previously
  // unresolved methods to see if we can match them now (so that future passes
  // using method profiles benefit)
  conf.process_unresolved_secondary_method_profile_lines();
  mgr.set_metric("~result~SecondaryMethodProfiles~",
                 conf.get_secondary_method_profiles().size());
  mgr.set_metric("~result~SecondaryMethodProfiles~unresolved~",
                 conf.get_secondary_method_profiles().unresolved_size());
}

void maybe_write_hashes_incoming(const ConfigFiles& conf, const Scope& scope) {
  if (conf.emit_incoming_hashes()) {
    TRACE(PM, 1, "Writing incoming hashes...");
    Timer t("Writing incoming hashes");
    std::ofstream hashes_file(conf.metafile(INCOMING_HASHES));
    hashing::print_classes(hashes_file, scope);
  }
}

void maybe_write_hashes_outgoing(const ConfigFiles& conf, const Scope& scope) {
  if (conf.emit_outgoing_hashes()) {
    TRACE(PM, 1, "Writing outgoing hashes...");
    Timer t("Writing outgoing hashes");
    std::ofstream hashes_file(conf.metafile(OUTGOING_HASHES));
    hashing::print_classes(hashes_file, scope);
  }
}

void maybe_write_env_seeds_file(const ConfigFiles& conf, const Scope& scope) {
  char* seeds_output_file = std::getenv("REDEX_SEEDS_FILE");
  if (seeds_output_file) {
    std::string seed_filename = seeds_output_file;
    Timer t("Writing seeds file " + seed_filename);
    std::ofstream seeds_file(seed_filename);
    keep_rules::print_seeds(seeds_file, conf.get_proguard_map(), scope, false,
                            false);
  }
}

void maybe_print_seeds_incoming(
    const ConfigFiles& conf,
    const Scope& scope,
    const std::unique_ptr<keep_rules::ProguardConfiguration>& pg_config) {
  if (!conf.get_printseeds().empty()) {
    Timer t("Writing seeds to file " + conf.get_printseeds());
    std::ofstream seeds_file(conf.get_printseeds());
    keep_rules::print_seeds(seeds_file, conf.get_proguard_map(), scope);
    std::ofstream config_file(conf.get_printseeds() + ".pro");
    redex_assert(pg_config != nullptr);
    keep_rules::show_configuration(config_file, scope, *pg_config);
    std::ofstream incoming(conf.get_printseeds() + ".incoming");
    redex::print_classes(incoming, conf.get_proguard_map(), scope);
    std::ofstream shrinking_file(conf.get_printseeds() + ".allowshrinking");
    keep_rules::print_seeds(shrinking_file, conf.get_proguard_map(), scope,
                            true, false);
    std::ofstream obfuscation_file(conf.get_printseeds() + ".allowobfuscation");
    keep_rules::print_seeds(obfuscation_file, conf.get_proguard_map(), scope,
                            false, true);
  }
}

void maybe_print_seeds_outgoing(const ConfigFiles& conf,
                                const DexStoreClassesIterator& it) {
  if (!conf.get_printseeds().empty()) {
    Timer t("Writing outgoing classes to file " + conf.get_printseeds() +
            ".outgoing");
    // Recompute the scope.
    auto scope = build_class_scope(it);
    std::ofstream outgoing(conf.get_printseeds() + ".outgoing");
    redex::print_classes(outgoing, conf.get_proguard_map(), scope);
  }
}

void maybe_enable_opt_data(const ConfigFiles& conf) {
  // Enable opt decision logging if specified in config.
  const Json::Value& opt_decisions_args =
      conf.get_json_config()["opt_decisions"];
  if (opt_decisions_args.get("enable_logs", false).asBool()) {
    opt_metadata::OptDataMapper::get_instance().enable_logs();
  }
}

bool is_run_hasher_after_each_pass(const ConfigFiles& conf,
                                   const RedexOptions& options) {
  if (options.disable_dex_hasher) {
    return false;
  }

  const Json::Value& hasher_args = conf.get_json_config()["hasher"];
  return hasher_args.get("run_after_each_pass", false).asBool();
}

void ensure_editable_cfg(DexStoresVector& stores) {
  auto temp_scope = build_class_scope(stores);
  walk::parallel::code(temp_scope, [&](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ true, /*fresh_editable_build*/ false);
  });
}

class AfterPassSizes {
 private:
  PassManager* m_mgr;

  // Would be nice to do things multi-threaded, but then we cannot
  // fork and can have only one job in flight. Instead store pids
  // and use non-blocking waits.
  struct Job {
    PassManager::PassInfo* pass_info;
    std::string tmp_dir;
    pid_t pid;
    Job(PassManager::PassInfo* pass_info, const std::string& tmp_dir, pid_t pid)
        : pass_info(pass_info), tmp_dir(tmp_dir), pid(pid) {}
  };
  std::list<Job> m_open_jobs;

  bool m_enabled{false};
  bool m_run_interdex{true};
  bool m_debug{false};
  size_t m_max_jobs{4};

 public:
  AfterPassSizes(PassManager* mgr, const ConfigFiles& conf) : m_mgr(mgr) {
    const auto& json = conf.get_json_config();
    m_enabled = json.get("after_pass_size", m_enabled);
    m_run_interdex = json.get("after_pass_size_interdex", m_run_interdex);
    m_debug = json.get("after_pass_size_debug", m_debug);
    json.get("after_pass_size_queue", m_max_jobs, m_max_jobs);
  }

  bool handle(PassManager::PassInfo* pass_info,
              DexStoresVector* stores,
              ConfigFiles* conf) {
    if (!m_enabled) {
      return false;
    }

#ifdef __linux__
    for (;;) {
      check_open_jobs(/*no_hang=*/true);
      if (m_open_jobs.size() < m_max_jobs) {
        break;
      }
      sleep(1); // Wait a bit.
    }

    // Create a temp dir.
    std::string tmp_dir;
    {
      auto tmp_path = boost::filesystem::temp_directory_path();
      tmp_path /= "redex.after_pass_size.XXXXXX";
      const auto& tmp_str = tmp_path.string();
      std::unique_ptr<char[]> c_str =
          std::make_unique<char[]>(tmp_str.length() + 1);
      strcpy(c_str.get(), tmp_str.c_str());
      char* dir_name = mkdtemp(c_str.get());
      if (dir_name == nullptr) {
        std::cerr << "Could not create temporary directory!";
        return false;
      }
      tmp_dir = dir_name;
    }

    pid_t p = fork();

    if (p < 0) {
      std::cerr << "Fork failed!" << strerror(errno) << std::endl;
      return false;
    }

    if (p > 0) {
      // Parent (=this).
      m_open_jobs.emplace_back(pass_info, tmp_dir, p);
      return false;
    }

    // Child.
    return handle_child(tmp_dir, stores, conf);
#else
    (void)pass_info;
    return false;
#endif
  }

  void wait() {
#ifdef __linux__
    check_open_jobs(/*no_hang=*/false);
#endif
  }

 private:
#ifdef __linux__
  void check_open_jobs(bool no_hang) {
    for (auto it = m_open_jobs.begin(); it != m_open_jobs.end();) {
      int stat;
      pid_t wait_res;
      for (;;) {
        wait_res = waitpid(it->pid, &stat, no_hang ? WNOHANG : 0);
        if (wait_res != -1 || errno != EINTR) {
          break;
        }
      }
      if (wait_res == 0) {
        // Not done.
        ++it;
        continue;
      }
      if (wait_res == -1) {
        std::cerr << "Failed " << it->pass_info->name << std::endl;
      } else {
        if (WIFEXITED(stat) && WEXITSTATUS(stat) == 0) {
          handle_parent(*it);
        } else {
          std::cerr << "AfterPass child failed: " << std::hex << stat
                    << std::dec << std::endl;
        }
      }
      boost::filesystem::remove_all(it->tmp_dir);
      it = m_open_jobs.erase(it);
    }
  }

  void handle_parent(const Job& job) {
    // Collect dex file sizes in the temp directory.
    // Discover dex files
    namespace fs = boost::filesystem;
    auto end = fs::directory_iterator();
    size_t sum{0};
    for (fs::directory_iterator it{job.tmp_dir}; it != end; ++it) {
      const auto& file = it->path();
      if (fs::is_regular_file(file) &&
          !file.extension().compare(std::string(".dex"))) {
        sum += fs::file_size(file);
      }
    }
    job.pass_info->metrics["after_pass_size"] = sum;
    if (m_debug) {
      std::cerr << "Got " << sum << " for " << job.pass_info->name << std::endl;
    }
  }

  bool handle_child(const std::string& tmp_dir,
                    DexStoresVector* stores,
                    ConfigFiles* conf) {
    // Change output directory.
    if (m_debug) {
      std::cerr << "After-pass-size to " << tmp_dir << std::endl;
    }
    conf->set_outdir(tmp_dir);

    // Close output. No noise. (Maybe make this configurable)
    if (!m_debug) {
      close(STDOUT_FILENO);
      close(STDERR_FILENO);
    }

    // Ensure that aborts work correctly.
    set_abort_if_not_this_thread();

    auto maybe_run = [&](const char* pass_name) {
      auto pass = m_mgr->find_pass(pass_name);
      if (pass != nullptr) {
        if (m_debug) {
          std::cerr << "Running " << pass_name << std::endl;
        }
        if (pass->is_editable_cfg_friendly()) {
          ensure_editable_cfg(*stores);
        }
        pass->run_pass(*stores, *conf, *m_mgr);
      }
    };

    // If configured with InterDexPass, better run that. Expensive, but may be
    // required for dex constraints.
    if (m_run_interdex && !m_mgr->interdex_has_run()) {
      maybe_run("InterDexPass");
    }
    // Better run MakePublicPass.
    maybe_run("MakePublicPass");
    // May need register allocation.
    if (!m_mgr->regalloc_has_run()) {
      maybe_run("RegAllocPass");
    }

    // Ensure we do not wait for anything copied from the parent.
    m_open_jobs.clear();
    m_enabled = false;

    // Make the PassManager skip further passes.
    return true;
  }
#endif
};

void run_assessor(PassManager& pm, const Scope& scope, bool initially = false) {
  TRACE(PM, 2, "Running assessor...");
  Timer t("Assessor");
  assessments::DexScopeAssessor assessor(scope);
  auto assessment = assessor.run();
  std::string prefix =
      std::string("~") + (initially ? "PRE" : "") + "assessment~";
  // log metric value in a way that fits into JSON number value
  for (auto& p : assessments::order(assessment)) {
    pm.set_metric(prefix + p.first, p.second);
  }
}

// For debugging purpose allows tracing a class after each pass.
// Env variable TRACE_CLASS_FILE provides the name of the output file where
// these data will be written and env variable TRACE_CLASS_NAME would provide
// the name of the class to be traced.
class TraceClassAfterEachPass {
 public:
  TraceClassAfterEachPass() {

    trace_class_file = getenv("TRACE_CLASS_FILE");
    trace_class_name = getenv("TRACE_CLASS_NAME");
    std::cerr << "TRACE_CLASS_FILE="
              << (trace_class_file == nullptr ? "" : trace_class_file)
              << std::endl;
    std::cerr << "TRACE_CLASS_NAME="
              << (trace_class_name == nullptr ? "" : trace_class_name)
              << std::endl;
    if (trace_class_name) {
      if (trace_class_file) {
        try {
          int int_fd = std::stoi(trace_class_file);
          fd = fdopen(int_fd, "w");
        } catch (std::invalid_argument&) {
          // Not an integer file descriptor; real file name.
          fd = fopen(trace_class_file, "w");
        }
        if (!fd) {
          fprintf(stderr,
                  "Unable to open TRACE_CLASS_FILE, falling back to stderr\n");
          fd = stderr;
        }
      }
    }
  }

  ~TraceClassAfterEachPass() {
    if (fd != stderr) {
      fclose(fd);
    }
  }

  void dump_cls(DexClass* cls) {
    fprintf(fd, "Class %s\n", SHOW(cls));
    std::vector<DexMethod*> methods = cls->get_all_methods();
    std::vector<DexField*> fields = cls->get_all_fields();
    for (auto* v : fields) {
      fprintf(fd, "Field %s\n", SHOW(v));
    }
    for (auto* v : methods) {
      fprintf(fd, "Method %s\n", SHOW(v));
      if (v->get_code()) {
        fprintf(fd, "%s\n", SHOW(v->get_code()));
      }
    }
  }

  void dump(const std::string& pass_name) {
    if (trace_class_name) {
      fprintf(fd, "After Pass  %s\n", pass_name.c_str());
      auto* typ = DexType::get_type(trace_class_name);
      if (typ && type_class(typ)) {
        dump_cls(type_class(typ));
      } else {
        fprintf(fd, "Class = %s not foud\n", trace_class_name);
      }
    }
  }

 private:
  FILE* fd = stderr;
  char* trace_class_file;
  char* trace_class_name;
};

static TraceClassAfterEachPass trace_cls;

struct JemallocStats {
  PassManager* pm;
  const ConfigFiles& c;
  bool full_stats{false};

  JemallocStats(PassManager* pm, const ConfigFiles& c) : pm(pm), c(c) {
    const auto* pmc =
        c.get_global_config().get_config_by_name<PassManagerConfig>(
            "pass_manager");
    redex_assert(pmc != nullptr);

    full_stats = pmc->jemalloc_full_stats;
  }

  void process_jemalloc_stats_for_pass(const Pass* pass, size_t run) {
#ifdef USE_JEMALLOC
    std::string key_base = "~jemalloc.";
    auto cb = [&](const char* key, uint64_t value) {
      pm->set_metric(key_base + key, value);
    };
    jemalloc_util::some_malloc_stats(cb);

    if (full_stats) {
      std::string name =
          "jemalloc." + pass->name() + "." + std::to_string(run) + ".json";
      auto filename = c.metafile(name);
      std::ofstream ofs{filename};
      ofs << jemalloc_util::get_malloc_stats();
    }
#endif
  }
};

} // namespace

std::unique_ptr<keep_rules::ProguardConfiguration> empty_pg_config() {
  return std::make_unique<keep_rules::ProguardConfiguration>();
}

struct PassManager::InternalFields {
  std::mutex m_metrics_lock;
};

PassManager::PassManager(const std::vector<Pass*>& passes)
    : PassManager(
          passes, ConfigFiles(Json::Value(Json::objectValue)), RedexOptions{}) {
}
PassManager::PassManager(const std::vector<Pass*>& passes,
                         const ConfigFiles& config,
                         const RedexOptions& options)
    : PassManager(passes, empty_pg_config(), config, options) {}

PassManager::PassManager(
    const std::vector<Pass*>& passes,
    std::unique_ptr<keep_rules::ProguardConfiguration> pg_config)
    : PassManager(passes,
                  std::move(pg_config),
                  ConfigFiles(Json::Value(Json::objectValue)),
                  RedexOptions{}) {}
PassManager::PassManager(
    const std::vector<Pass*>& passes,
    std::unique_ptr<keep_rules::ProguardConfiguration> pg_config,
    const ConfigFiles& config,
    const RedexOptions& options)
    : m_asset_mgr(get_apk_dir(config)),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(std::move(pg_config)),
      m_redex_options(options),
      m_testing_mode(false),
      m_internal_fields(new InternalFields()) {
  init(config);
  if (getenv("MALLOC_PROFILE_PASS")) {
    m_malloc_profile_pass = find_pass(getenv("MALLOC_PROFILE_PASS"));
    always_assert(m_malloc_profile_pass != nullptr);
    fprintf(stderr, "Will run jemalloc profiler for %s\n",
            m_malloc_profile_pass->name().c_str());
  }
}

PassManager::~PassManager() {}

void PassManager::init(const ConfigFiles& config) {
  if (config.get_json_config().contains("redex") &&
      config.get_json_config().get("redex", Json::Value()).isMember("passes")) {
    PassManagerConfig default_config;
    auto& pm_config = [&]() -> PassManagerConfig& {
      if (!config.get_global_config().has_config_by_name("pass_manager")) {
        return default_config;
      }
      return *config.get_global_config().get_config_by_name<PassManagerConfig>(
          "pass_manager");
    }();
    auto get_alias = [pm_config](const auto& name) -> const std::string* {
      auto it = pm_config.pass_aliases.find(name);
      if (it == pm_config.pass_aliases.end()) {
        return nullptr;
      }
      return &it->second;
    };

    const auto& json_config = config.get_json_config();
    const auto& passes_from_config = json_config["redex"]["passes"];
    for (const auto& pass : passes_from_config) {
      std::string pass_name = pass.asString();

      // Check whether it is explicitly disabled.
      auto is_disabled = [&json_config, &pass_name]() {
        if (!json_config.contains(pass_name.c_str())) {
          return false;
        }
        const auto& pass_data = json_config[pass_name.c_str()];
        if (!pass_data.isMember("disabled")) {
          return false;
        }
        return pass_data["disabled"].asBool();
      };
      if (is_disabled()) {
        continue;
      }

      activate_pass(pass_name, get_alias(pass_name),
                    config.get_json_config().unwrap());
    }
  } else {
    // If config isn't set up, run all registered passes.
    m_activated_passes = m_registered_passes;
    // But do not forget to initialize them.
    const auto& json_config = config.get_json_config();
    for (auto* pass : m_activated_passes) {
      pass->parse_config(JsonWrapper(json_config[pass->name().c_str()]));
    }
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
    m_pass_info[i].config =
        JsonWrapper(config.get_json_config()[pass->name().c_str()]);
  }
}

hashing::DexHash PassManager::run_hasher(const char* pass_name,
                                         const Scope& scope) {
  TRACE(PM, 2, "Running hasher...");
  Timer t("Hasher");
  auto timer = m_hashers_timer.scope();
  hashing::DexScopeHasher hasher(scope);
  auto hash = hasher.run();
  if (pass_name) {
    // log metric value in a way that fits into JSON number value
    set_metric("~result~code~hash~",
               hash.code_hash & ((((size_t)1) << 52) - 1));
    set_metric("~result~registers~hash~",
               hash.registers_hash & ((((size_t)1) << 52) - 1));
    set_metric("~result~positions~hash~",
               hash.positions_hash & ((((size_t)1) << 52) - 1));
    set_metric("~result~signature~hash~",
               hash.signature_hash & ((((size_t)1) << 52) - 1));
  }
  auto positions_hash_string = hashing::hash_to_string(hash.positions_hash);
  auto registers_hash_string = hashing::hash_to_string(hash.registers_hash);
  auto code_hash_string = hashing::hash_to_string(hash.code_hash);
  auto signature_hash_string = hashing::hash_to_string(hash.signature_hash);
  TRACE(PM, 3,
        "[scope hash] %s: positions#%s, registers#%s, code#%s, signature#%s",
        pass_name ? pass_name : "(initial)", positions_hash_string.c_str(),
        registers_hash_string.c_str(), code_hash_string.c_str(),
        signature_hash_string.c_str());
  return hash;
}

void PassManager::eval_passes(DexStoresVector& stores, ConfigFiles& conf) {
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Evaluating %s...", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    m_current_pass_info = &m_pass_info[i];
    pass->eval_pass(stores, conf, *this);
    m_current_pass_info = nullptr;
  }
}

void PassManager::run_passes(DexStoresVector& stores, ConfigFiles& conf) {
  auto profiler_info = ScopedCommandProfiling::maybe_info_from_env("");
  const Pass* profiler_info_pass = nullptr;
  if (profiler_info) {
    profiler_info_pass = get_profiled_pass(*this);
  }
  auto profiler_all_info =
      ScopedCommandProfiling::maybe_info_from_env("ALL_PASSES_");

  if (conf.force_single_dex()) {
    // Squash the dexes into one, so that the passes all see only one dex and
    // all the cross-dex reference checking are accurate.
    squash_into_one_dex(stores);
  }

  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);

  // Clear stale data. Make sure we start fresh.
  m_preserved_analysis_passes.clear();

  {
    Timer t("API Level Checker");
    api::LevelChecker::init(m_redex_options.min_sdk, scope);
  }

  maybe_write_env_seeds_file(conf, scope);
  maybe_print_seeds_incoming(conf, scope, m_pg_config);
  maybe_write_hashes_incoming(conf, scope);

  maybe_enable_opt_data(conf);

  // Load configurations regarding the scope.
  conf.load(scope);

  sanitizers::lsan_do_recoverable_leak_check();

  eval_passes(stores, conf);

  // Retrieve the hasher's settings.
  bool run_hasher_after_each_pass =
      is_run_hasher_after_each_pass(conf, get_redex_options());

  // Retrieve the assessor's settings.
  const auto* assessor_config =
      conf.get_global_config().get_config_by_name<AssessorConfig>("assessor");

  // Retrieve the type checker's settings.
  CheckerConfig checker_conf{conf};
  checker_conf.on_input(scope);

  // Pull on method-profiles, so that they get initialized, and are matched
  // against the *initial* methods
  conf.get_method_profiles();

  if (run_hasher_after_each_pass) {
    m_initial_hash = run_hasher(nullptr, scope);
  }

  CheckUniqueDeobfuscatedNames check_unique_deobfuscated{conf};
  check_unique_deobfuscated.run_initially(scope);

  VisualizerHelper graph_visualizer(conf);

  sanitizers::lsan_do_recoverable_leak_check();

  const bool mem_pass_stats =
      traceEnabled(STATS, 1) || conf.get_json_config().get("mem_stats", true);
  const bool hwm_per_pass =
      conf.get_json_config().get("mem_stats_per_pass", true);

  size_t min_pass_idx_for_dex_ref_check =
      checker_conf.min_pass_idx_for_dex_ref_check(m_activated_passes);

  // Abort if the analysis pass dependencies are not satisfied.
  AnalysisUsage::check_dependencies(m_activated_passes);

  AfterPassSizes after_pass_size(this, conf);

  // For core loop legibility, have a lambda here.

  auto pre_pass_verifiers = [&](Pass* pass, size_t i) {
    if (i == 0 && assessor_config->run_initially) {
      ::run_assessor(*this, scope, /* initially */ true);
    }
  };

  auto post_pass_verifiers = [&](Pass* pass, size_t i, size_t size) {
    ConcurrentSet<const DexMethodRef*> all_code_referenced_methods;
    ConcurrentSet<DexMethod*> unique_methods;
    bool is_editable_cfg_friendly = pass->is_editable_cfg_friendly();
    walk::parallel::code(build_class_scope(stores), [&](DexMethod* m,
                                                        IRCode& code) {
      if (is_editable_cfg_friendly) {
        always_assert_log(code.editable_cfg_built(), "%s has a cfg!", SHOW(m));
      }
      if (slow_invariants_debug) {
        std::vector<DexMethodRef*> methods;
        methods.reserve(1000);
        methods.push_back(m);
        code.gather_methods(methods);
        for (auto* mref : methods) {
          always_assert_log(
              DexMethod::get_method(mref->get_class(), mref->get_name(),
                                    mref->get_proto()) != nullptr,
              "Did not find %s in the context, referenced from %s!", SHOW(mref),
              SHOW(m));
          all_code_referenced_methods.insert(mref);
        }
        if (!unique_methods.insert(m)) {
          not_reached_log("Duplicate method: %s", SHOW(m));
        }
      }
    });
    if (slow_invariants_debug) {
      ScopedMetrics sm(*this);
      sm.set_metric("num_code_referenced_methods",
                    all_code_referenced_methods.size());
    }

    bool run_hasher = run_hasher_after_each_pass;
    bool run_assessor = assessor_config->run_after_each_pass ||
                        (assessor_config->run_finally && i == size - 1);
    bool run_type_checker = checker_conf.run_after_pass(pass);

    if (run_hasher || run_assessor || run_type_checker ||
        check_unique_deobfuscated.m_after_each_pass) {
      scope = build_class_scope(it);

      if (run_hasher) {
        m_current_pass_info->hash = boost::optional<hashing::DexHash>(
            this->run_hasher(pass->name().c_str(), scope));
      }
      if (run_assessor) {
        ::run_assessor(*this, scope);
        ScopedMetrics sm(*this);
        source_blocks::track_source_block_coverage(sm, stores);
      }
      if (run_type_checker) {
        // It's OK to overwrite the `this` register if we are not yet at the
        // output phase -- the register allocator can fix it up later.
        checker_conf.check_no_overwrite_this(false)
            .validate_access(false)
            .run_verifier(scope);
      }
      auto timer = m_check_unique_deobfuscateds_timer.scope();
      check_unique_deobfuscated.run_after_pass(pass, scope);
    }
    if (i >= min_pass_idx_for_dex_ref_check) {
      CheckerConfig::ref_validation(stores, pass->name());
    }
  };

  JNINativeContextHelper jni_native_context_helper(
      scope, m_redex_options.jni_summary_path);

  JemallocStats jemalloc_stats{this, conf};

  std::unordered_map<const Pass*, size_t> runs;

  /////////////////////
  // MAIN PASS LOOP. //
  /////////////////////
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    const size_t pass_run = ++runs[pass];
    AnalysisUsageHelper analysis_usage_helper{m_preserved_analysis_passes};
    analysis_usage_helper.pre_pass(pass);

    TRACE(PM, 1, "Running %s...", pass->name().c_str());
    ScopedMemStats scoped_mem_stats{mem_pass_stats, hwm_per_pass};
    Timer t(pass->name() + " " + std::to_string(pass_run) + " (run)");
    m_current_pass_info = &m_pass_info[i];

    pre_pass_verifiers(pass, i);

    {
      auto scoped_command_prof = profiler_info_pass == pass
                                     ? ScopedCommandProfiling::maybe_from_info(
                                           profiler_info, &pass->name())
                                     : boost::none;
      auto scoped_command_all_prof = ScopedCommandProfiling::maybe_from_info(
          profiler_all_info, &pass->name());
      jemalloc_util::ScopedProfiling malloc_prof(m_malloc_profile_pass == pass);
      if (!pass->is_editable_cfg_friendly()) {
        // if this pass hasn't been updated to editable_cfg yet, clear_cfg. In
        // the future, once all editable cfg updates are done, this branch will
        // be removed.
        auto temp_scope = build_class_scope(stores);
        walk::parallel::code(
            temp_scope, [&](DexMethod*, IRCode& code) { code.clear_cfg(); });
        TRACE(PM, 2, "%s Pass has not been updated to editable cfg.\n",
              SHOW(pass->name()));
      } else {
        // Run build_cfg() in case any newly added methods by previous passes
        // are not built as editable cfg. But if editable cfg is already built,
        // no need to rebuild it.
        ensure_editable_cfg(stores);
        TRACE(PM, 2, "%s Pass uses editable cfg.\n", SHOW(pass->name()));
      }
      pass->run_pass(stores, conf, *this);

      // Ensure the CFG is clean, e.g., no unreachable blocks.
      if (pass->is_editable_cfg_friendly()) {
        auto temp_scope = build_class_scope(stores);
        walk::parallel::code(temp_scope, [&](DexMethod* method, IRCode& code) {
          always_assert_log(code.editable_cfg_built(),
                            "%s has no editable cfg after cfg-friendly pass %s",
                            SHOW(method), pass->name().c_str());
          code.cfg().simplify();
        });
      }

      trace_cls.dump(pass->name());
    }

    scoped_mem_stats.trace_log(this, pass);

    jemalloc_stats.process_jemalloc_stats_for_pass(pass, pass_run);

    sanitizers::lsan_do_recoverable_leak_check();

    graph_visualizer.add_pass(pass, i);

    post_pass_verifiers(pass, i, m_activated_passes.size());

    analysis_usage_helper.post_pass(pass);

    process_method_profiles(*this, conf);
    process_secondary_method_profiles(*this, conf);

    if (after_pass_size.handle(m_current_pass_info, &stores, &conf)) {
      // Measuring child. Return to write things out.
      break;
    }

    m_current_pass_info = nullptr;
  }

  after_pass_size.wait();

  // Always clear cfg and run the type checker before generating the optimized
  // dex code.
  scope = build_class_scope(it);
  walk::parallel::code(scope,
                       [&](DexMethod*, IRCode& code) { code.clear_cfg(); });
  TRACE(PM, 1, "All opt passes are done, clear cfg\n");
  checker_conf.check_no_overwrite_this(get_redex_options().no_overwrite_this())
      .validate_access(true)
      .run_verifier(scope);

  jni_native_context_helper.post_passes(scope, conf);

  check_unique_deobfuscated.run_finally(scope);

  graph_visualizer.finalize();

  maybe_print_seeds_outgoing(conf, it);
  maybe_write_hashes_outgoing(conf, scope);

  sanitizers::lsan_do_recoverable_leak_check();

  Timer::add_timer("PassManager.Hashers", m_hashers_timer.get_seconds());
  Timer::add_timer("PassManager.CheckUniqueDeobfuscateds",
                   m_check_unique_deobfuscateds_timer.get_seconds());
  Timer::add_timer("CFGMutation", cfg::CFGMutation::get_seconds());
  Timer::add_timer(
      "MethodProfiles::process_unresolved_lines",
      method_profiles::MethodProfiles::get_process_unresolved_lines_seconds());
  Timer::add_timer("compute_locations_closure_wto",
                   get_compute_locations_closure_wto_seconds());
}

void PassManager::activate_pass(const std::string& name,
                                const std::string* alias,
                                const Json::Value& conf) {
  // Names may or may not have a "#<id>" suffix to indicate their order in the
  // pass list, which needs to be removed for matching.
  auto activate = [this, &conf](const std::string& n, const std::string* a) {
    for (auto pass : m_registered_passes) {
      if (n == pass->name()) {
        if (a != nullptr) {
          auto cloned_pass = pass->clone(*a);
          always_assert_log(cloned_pass != nullptr,
                            "Cannot clone pass %s to make alias %s", n.c_str(),
                            a->c_str());
          pass = cloned_pass.get();
          m_cloned_passes.emplace_back(std::move(cloned_pass));
        }

        m_activated_passes.push_back(pass);

        // Retrieving the configuration specific to this particular run
        // of the pass.
        pass->parse_config(JsonWrapper(conf[a == nullptr ? n : *a]));
        return true;
      }
    }
    return false;
  };

  // Does a pass exist with this name (directly)?
  if (activate(name, nullptr)) {
    return;
  }

  // Can we find it under the given alias?
  if (alias != nullptr && activate(*alias, &name)) {
    return;
  }

  not_reached_log("No pass named %s(%s)!", name.c_str(),
                  alias != nullptr ? alias->c_str() : "n/a");
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
  std::unique_lock<std::mutex> lock{m_internal_fields->m_metrics_lock};
  (m_current_pass_info->metrics)[key] += value;
}

void PassManager::set_metric(const std::string& key, int64_t value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  std::unique_lock<std::mutex> lock{m_internal_fields->m_metrics_lock};
  (m_current_pass_info->metrics)[key] = value;
}

int64_t PassManager::get_metric(const std::string& key) {
  std::unique_lock<std::mutex> lock{m_internal_fields->m_metrics_lock};
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
