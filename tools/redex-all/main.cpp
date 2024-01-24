/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "json/value.h"
#include <boost/thread/thread.hpp>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <regex>
#include <set>
#include <streambuf>
#include <string>
#include <vector>

#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <json/json.h>

#include "AggregateException.h"
#include "CommandProfiling.h"
#include "CommentFilter.h"
#include "ConfigFiles.h"
#include "ControlFlow.h" // To set DEBUG.
#include "Debug.h"
#include "DebugUtils.h"
#include "DexClass.h"
#include "DexHasher.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexPosition.h"
#include "DuplicateClasses.h"
#include "GlobalConfig.h"
#include "IODIMetadata.h"
#include "InstructionLowering.h"
#include "JarLoader.h"
#include "JemallocUtil.h"
#include "KeepReason.h"
#include "Macros.h"
#include "MallocDebug.h"
#include "MonitorCount.h"
#include "NoOptimizationsMatcher.h"
#include "OptData.h"
#include "PassRegistry.h"
#include "PostLowering.h"
#include "ProguardConfiguration.h" // New ProGuard configuration
#include "ProguardMatcher.h"
#include "ProguardParser.h" // New ProGuard Parser
#include "ProguardPrintConfiguration.h" // New ProGuard configuration
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "RedexProperties.h"
#include "RedexPropertiesManager.h"
#include "RedexPropertyCheckerRegistry.h"
#include "RedexResources.h"
#include "Sanitizers.h"
#include "SanitizersConfig.h"
#include "ScopedMemStats.h"
#include "Show.h"
#include "ThreadPool.h"
#include "Timer.h"
#include "ToolsCommon.h"
#include "Walkers.h"
#include "Warning.h"
#include "WorkQueue.h" // For concurrency.

namespace {

// Do *not* change these values. Many services will break.
constexpr const char* LINE_NUMBER_MAP = "redex-line-number-map-v2";
constexpr const char* DEBUG_LINE_MAP = "redex-debug-line-map-v2";
constexpr const char* IODI_METADATA = "iodi-metadata";
constexpr const char* OPT_DECISIONS = "redex-opt-decisions.json";
constexpr const char* CLASS_METHOD_INFO_MAP = "redex-class-method-info-map.txt";

const std::string k_usage_header = "usage: redex-all [options...] dex-files...";

void print_usage() {
  std::cout << k_usage_header << std::endl;
  std::cout << "Try 'redex-all -h' for more information." << std::endl;
}

struct Arguments {
  Json::Value config{Json::nullValue};
  std::set<std::string> jar_paths;
  std::vector<std::string> proguard_config_paths;
  std::string out_dir;
  std::vector<std::string> dex_files;
  // Entry data contains the list of dex files, config file and original
  // command line arguments. For development usage
  Json::Value entry_data;
  boost::optional<int> stop_pass_idx;
  RedexOptions redex_options;
  bool properties_check{false};
  bool properties_check_allow_disabled{false};
};

UNUSED void dump_args(const Arguments& args) {
  std::cout << "out_dir: " << args.out_dir << std::endl;
  std::cout << "verify_none_mode: " << args.redex_options.verify_none_enabled
            << std::endl;
  std::cout << "art_build: " << args.redex_options.is_art_build << std::endl;
  std::cout << "enable_instrument_pass: "
            << args.redex_options.instrument_pass_enabled << std::endl;
  std::cout << "min_sdk: " << args.redex_options.min_sdk << std::endl;
  std::cout << "debug_info_kind: "
            << debug_info_kind_to_string(args.redex_options.debug_info_kind)
            << std::endl;
  std::cout << "jar_paths: " << std::endl;
  for (const auto& e : args.jar_paths) {
    std::cout << "  " << e << std::endl;
  }
  std::cout << "proguard_config_paths: " << std::endl;
  for (const auto& e : args.proguard_config_paths) {
    std::cout << "  " << e << std::endl;
  }
  std::cout << "dex_files: " << std::endl;
  for (const auto& e : args.dex_files) {
    std::cout << "  " << e << std::endl;
  }
  std::cout << "config: " << std::endl;
  std::cout << args.config << std::endl;
  std::cout << "arch: " << std::endl;
  std::cout << args.redex_options.arch << std::endl;
}

Json::Value parse_json_value(const std::string& value_string) {
  std::istringstream temp_stream(value_string);
  Json::Value temp_json;
  temp_stream >> temp_json;
  return temp_json;
}

bool add_value_to_config(Json::Value& config,
                         const std::string& key_value,
                         bool is_json) {
  const size_t equals_idx = key_value.find('=');
  if (equals_idx == std::string::npos) {
    return false;
  }

  const size_t dot_idx = key_value.find('.');
  if (dot_idx != std::string::npos && dot_idx < equals_idx) {
    // Pass-specific config value specified with -Dpassname.key=value
    std::string pass = key_value.substr(0, dot_idx);
    std::string key = key_value.substr(dot_idx + 1, equals_idx - dot_idx - 1);
    std::string value_string = key_value.substr(equals_idx + 1);
    if (is_json) {
      config[pass][key] = parse_json_value(value_string);
    } else {
      config[pass][key] = value_string;
    }
  } else {
    // Global config value specified with -Dkey=value
    std::string key = key_value.substr(0, equals_idx);
    std::string value_string = key_value.substr(equals_idx + 1);
    if (is_json) {
      config[key] = parse_json_value(value_string);
    } else {
      config[key] = value_string;
    }
  }
  return true;
}

Json::Value default_config() {
  const auto passes = {
      "ReBindRefsPass",   "BridgeSynthInlinePass", "FinalInlinePassV2",
      "DelSuperPass",     "SingleImplPass",        "MethodInlinePass",
      "StaticReloPassV2", "ShortenSrcStringsPass", "RegAllocPass",
  };
  std::istringstream temp_json("{\"redex\":{\"passes\":[]}}");
  Json::Value cfg;
  temp_json >> cfg;
  for (auto const& pass : passes) {
    cfg["redex"]["passes"].append(pass);
  }
  return cfg;
}

Json::Value reflect_config(const Configurable::Reflection& cr) {
  Json::Value params = Json::arrayValue;
  Json::Value traits = Json::arrayValue;
  int params_idx = 0;
  for (auto& entry : cr.params) {
    Json::Value param;
    param["name"] = entry.first;
    param["doc"] = entry.second.doc;
    param["is_required"] = entry.second.is_required;
    param["bindflags"] = static_cast<Json::UInt64>(entry.second.bindflags);
    [&]() {
      switch (entry.second.type) {
      case Configurable::ReflectionParam::Type::PRIMITIVE:
        param["type"] =
            std::get<Configurable::ReflectionParam::Type::PRIMITIVE>(
                entry.second.variant);
        param["default_value"] = entry.second.default_value;
        return;
      case Configurable::ReflectionParam::Type::COMPOSITE:
        param["type"] = reflect_config(
            std::get<Configurable::ReflectionParam::Type::COMPOSITE>(
                entry.second.variant));
        return;
      }
      not_reached_log("Invalid Configurable::ReflectionParam::Type: %d",
                      entry.second.type);
    }();
    params[params_idx++] = param;
  }
  int traits_idx = 0;
  for (auto& entry : cr.traits) {
    Json::Value trait;
    trait["name"] = entry.first;
    trait["value"] = entry.second.value;
    traits[traits_idx++] = trait;
  }
  Json::Value reflected_config;
  reflected_config["name"] = cr.name;
  reflected_config["doc"] = cr.doc;
  reflected_config["params"] = params;
  reflected_config["traits"] = traits;
  return reflected_config;
}

void add_pass_properties_reflection(Json::Value& value, Pass* pass) {
  using namespace redex_properties;
  auto interactions = pass->get_property_interactions();
  if (interactions.empty()) {
    return;
  }

  Json::Value establishes = Json::arrayValue;
  Json::Value requires_ = Json::arrayValue;
  Json::Value preserves = Json::arrayValue;
  Json::Value requires_finally = Json::arrayValue;

  for (const auto& [property, inter] : interactions) {
    if (inter.establishes) {
      establishes.append(get_name(property));
    }
    if (inter.requires_) {
      requires_.append(get_name(property));
    }
    if (inter.preserves) {
      preserves.append(get_name(property));
    }
    if (inter.requires_finally) {
      requires_finally.append(get_name(property));
    }
  }

  Json::Value properties;
  properties["establishes"] = establishes;
  properties["requires"] = requires_;
  properties["preserves"] = preserves;
  properties["requires_finally"] = requires_finally;

  value["properties"] = properties;
}

Json::Value reflect_property_definitions() {
  Json::Value properties;

  properties["properties"] = []() {
    Json::Value prop_map;
    auto all = redex_properties::get_all_properties();
    for (auto& prop : all) {
      Json::Value prop_value;
      prop_value["negative"] = redex_properties::is_negative(prop);
      prop_map[redex_properties::get_name(prop)] = std::move(prop_value);
    }
    return prop_map;
  }();

  auto create_sorted = [](const auto& input) {
    std::vector<redex_properties::Property> tmp;
    std::copy(input.begin(), input.end(), std::back_inserter(tmp));
    std::sort(tmp.begin(), tmp.end());

    Json::Value holder = Json::arrayValue;
    for (auto& prop : tmp) {
      holder.append(redex_properties::get_name(prop));
    }
    return holder;
  };

  properties["initial"] =
      create_sorted(redex_properties::Manager::get_default_initial());
  properties["final"] =
      create_sorted(redex_properties::Manager::get_default_final());

  return properties;
}

Arguments parse_args(int argc, char* argv[]) {
  Arguments args;
  args.out_dir = ".";
  args.config = default_config();

  namespace po = boost::program_options;
  po::options_description od(k_usage_header);
  od.add_options()("help,h", "print this help message");
  od.add_options()("reflect-config",
                   "print a reflection of the config and exit");
  od.add_options()(
      "properties-check",
      "parse configuration, perform a stack properties check and exit");
  od.add_options()("properties-check-allow-disabled",
                   "accept the disable flag in the configuration");
  od.add_options()("apkdir,a",
                   // We allow overwrites to most of the options but will take
                   // only the last one.
                   po::value<std::vector<std::string>>(),
                   "directory containing unzipped APK");
  od.add_options()("config,c",
                   po::value<std::vector<std::string>>(),
                   "JSON-formatted config file");
  od.add_options()("outdir,o",
                   po::value<std::vector<std::string>>(),
                   "output directory for optimized dexes");
  od.add_options()(
      // This option value will be accumulated to a vector.
      "jarpath,j",
      po::value<std::vector<std::string>>(),
      "classpath jar");
  od.add_options()("proguard-config,p",
                   po::value<std::vector<std::string>>(), // Accumulation
                   "ProGuard config file");
  od.add_options()("printseeds,q",
                   po::value<std::vector<std::string>>(),
                   "file to report seeds computed by redex");
  od.add_options()(
      "used-js-assets", po::value<std::vector<std::string>>(),
      "A JSON file (or files) containing a list of resources used by JS");
  od.add_options()("warn,w",
                   po::value<std::vector<int>>(),
                   "warning level:\n"
                   "  0: no warnings\n"
                   "  1: count of warnings\n"
                   "  2: full text of warnings");
  od.add_options()(
      "verify-none-mode",
      po::bool_switch(&args.redex_options.verify_none_enabled)
          ->default_value(false),
      "run redex in verify-none mode\n"
      "  \tThis will activate optimization passes or code in some passes that "
      "wouldn't normally operate with verification enabled.");
  od.add_options()(
      "is-art-build",
      po::bool_switch(&args.redex_options.is_art_build)->default_value(false),
      "If specified, states that the current build is art specific.\n");
  od.add_options()(
      "disable-dex-hasher",
      po::bool_switch(&args.redex_options.disable_dex_hasher)
          ->default_value(false),
      "If specified, states that the current run disables dex hasher.\n");
  od.add_options()(
      "post-lowering",
      po::bool_switch(&args.redex_options.post_lowering)->default_value(false),
      "If specified, post lowering steps are run.\n");
  od.add_options()(
      "arch,A",
      po::value<std::vector<std::string>>(),
      "Architecture; one of arm/arm64/thumb2/x86_64/x86/mips/mips64");
  od.add_options()("enable-instrument-pass",
                   po::bool_switch(&args.redex_options.instrument_pass_enabled)
                       ->default_value(false),
                   "If specified, enables InstrumentPass if any.\n");
  od.add_options()(",S",
                   po::value<std::vector<std::string>>(), // Accumulation
                   "-Skey=string\n"
                   "  \tAdd a string value to the global config, overwriting "
                   "the existing value if any\n"
                   "    \te.g. -Smy_param_name=foo\n"
                   "-Spass_name.key=string\n"
                   "  \tAdd a string value to a pass"
                   "config, overwriting the existing value if any\n"
                   "    \te.g. -SMyPass.config=\"foo bar\"");
  od.add_options()(
      ",J",
      po::value<std::vector<std::string>>(), // Accumulation
      "-Jkey=<json value>\n"
      "  \tAdd a json value to the global config, overwriting the existing "
      "value if any\n"
      "    \te.g. -Jmy_param_name={\"foo\": true}\n"
      "-JSomePassName.key=<json value>\n"
      "  \tAdd a json value to a pass config, overwriting the existing value "
      "if any\n"
      "    \te.g. -JMyPass.config=[1, 2, 3]\n"
      "Note: Be careful to properly escape JSON parameters, e.g., strings must "
      "be quoted.");
  od.add_options()("show-passes", "show registered passes");
  od.add_options()("dex-files", po::value<std::vector<std::string>>(),
                   "dex files");

  // Development usage only, and Python script will generate the following
  // arguments.
  od.add_options()("stop-pass", po::value<int>(),
                   "Stop before pass n and output IR to file");
  od.add_options()("output-ir", po::value<std::string>(),
                   "IR output directory, used with --stop-pass");
  od.add_options()("jni-summary",
                   po::value<std::string>(),
                   "Path to JNI summary directory of json files.");
  po::positional_options_description pod;
  pod.add("dex-files", -1);
  po::variables_map vm;

  try {
    po::store(
        po::command_line_parser(argc, argv).options(od).positional(pod).run(),
        vm);
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }

  // -h, --help handling must be the first.
  if (vm.count("help")) {
    od.print(std::cout);
    exit(EXIT_SUCCESS);
  }

  // --reflect-config handling must be next
  if (vm.count("reflect-config")) {
    Json::Value reflected_config;

    GlobalConfig gc(GlobalConfig::default_registry());
    reflected_config["global"] = reflect_config(gc.reflect());

    Json::Value pass_configs = Json::arrayValue;
    const auto& passes = PassRegistry::get().get_passes();
    for (size_t i = 0; i < passes.size(); ++i) {
      auto& pass = passes[i];
      pass_configs[static_cast<int>(i)] = reflect_config(pass->reflect());
      add_pass_properties_reflection(pass_configs[static_cast<int>(i)], pass);
    }
    reflected_config["passes"] = pass_configs;

    reflected_config["properties"] = reflect_property_definitions();

    std::cout << reflected_config << std::flush;
    exit(EXIT_SUCCESS);
  }

  if (vm.count("show-passes")) {
    const auto& passes = PassRegistry::get().get_passes();
    std::cout << "Registered passes: " << passes.size() << std::endl;
    for (size_t i = 0; i < passes.size(); ++i) {
      std::cout << i + 1 << ": " << passes[i]->name() << std::endl;
    }
    exit(EXIT_SUCCESS);
  }

  if (vm.count("properties-check")) {
    args.properties_check = true;
  }
  if (vm.count("properties-check-allow-disabled")) {
    args.properties_check_allow_disabled = true;
  }

  if (vm.count("dex-files")) {
    args.dex_files = vm["dex-files"].as<std::vector<std::string>>();
  } else if (!args.properties_check) {
    std::cerr << "error: no input dex files" << std::endl << std::endl;
    print_usage();
    exit(EXIT_SUCCESS);
  }

  if (vm.count("warn")) {
    const auto& warns = vm["warn"].as<std::vector<int>>();
    for (int warn : warns) {
      if (!(0 <= warn && warn <= 2)) {
        std::cerr << "warning: ignoring invalid warning level option: " << warn
                  << std::endl;
      }
    }
    g_warning_level = OptWarningLevel(warns.back());
  }

  auto take_last = [](const auto& value) {
    return value.template as<std::vector<std::string>>().back();
  };

  if (vm.count("config")) {
    const std::string& config_file = take_last(vm["config"]);
    args.entry_data["config"] =
        boost::filesystem::absolute(config_file).string();
    args.config = redex::parse_config(config_file);
  }

  if (vm.count("outdir")) {
    args.out_dir = take_last(vm["outdir"]);
    if (!redex::dir_is_writable(args.out_dir)) {
      std::cerr << "error: outdir is not a writable directory: " << args.out_dir
                << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  if (vm.count("proguard-config")) {
    args.proguard_config_paths =
        vm["proguard-config"].as<std::vector<std::string>>();
  }

  if (vm.count("jarpath")) {
    const auto& jar_paths = vm["jarpath"].as<std::vector<std::string>>();
    for (const auto& e : jar_paths) {
      TRACE(MAIN, 2, "Command line -j option: %s", e.c_str());
      args.jar_paths.emplace(e);
    }
  }

  // We add these values to the config at the end so that they will always
  // overwrite values read from the config file regardless of the order of
  // arguments.
  if (vm.count("apkdir")) {
    args.entry_data["apk_dir"] = args.config["apk_dir"] =
        take_last(vm["apkdir"]);
  }

  if (vm.count("printseeds")) {
    args.config["printseeds"] = take_last(vm["printseeds"]);
  }

  if (vm.count("used-js-assets")) {
    const auto& js_assets_lists =
        vm["used-js-assets"].as<std::vector<std::string>>();
    Json::Value array(Json::arrayValue);
    for (const auto& list : js_assets_lists) {
      array.append(list);
    }
    args.config["used-js-assets"] = array;
  }

  if (vm.count("arch")) {
    std::string arch = take_last(vm["arch"]);
    args.redex_options.arch = parse_architecture(arch);
    if (args.redex_options.arch == Architecture::UNKNOWN) {
      std::cerr << "warning: cannot architecture " << arch << std::endl;
    }
  }

  if (vm.count("-S")) {
    for (auto& key_value : vm["-S"].as<std::vector<std::string>>()) {
      if (!add_value_to_config(args.config, key_value, false)) {
        std::cerr << "warning: cannot parse -S" << key_value << std::endl;
      }
    }
  }

  if (vm.count("-J")) {
    for (auto& key_value : vm["-J"].as<std::vector<std::string>>()) {
      if (!add_value_to_config(args.config, key_value, true)) {
        std::cerr << "warning: cannot parse -J" << key_value << std::endl;
      }
    }
  }

  args.redex_options.debug_info_kind =
      parse_debug_info_kind(args.config.get("debug_info_kind", "").asString());

  // Development usage only
  if (vm.count("stop-pass")) {
    args.stop_pass_idx = vm["stop-pass"].as<int>();
  }

  if (vm.count("output-ir")) {
    // The out_dir is for final apk only or intermediate results only.
    always_assert(args.stop_pass_idx);
    args.out_dir = vm["output-ir"].as<std::string>();
  }

  if (vm.count("jni-summary")) {
    args.redex_options.jni_summary_path = vm["jni-summary"].as<std::string>();
  }

  if (args.stop_pass_idx != boost::none) {
    // Resize the passes list and append an additional RegAllocPass if its final
    // pass is not RegAllocPass.
    auto& passes_list = args.config["redex"]["passes"];
    int idx = *args.stop_pass_idx;
    if (idx < 0 || (size_t)idx > passes_list.size()) {
      std::cerr << "Invalid stop_pass value\n";
      exit(EXIT_FAILURE);
    }
    if (passes_list.size() > (size_t)idx) {
      passes_list.resize(idx);
    }
    // Append the two passes when `--stop-pass` is enabled.
    passes_list.append("MakePublicPass");
    passes_list.append("RegAllocPass");
    if (args.out_dir.empty() || !redex::dir_is_writable(args.out_dir)) {
      std::cerr << "output-ir is empty or not writable" << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  std::string metafiles = args.out_dir + "/meta/";
  int status = [&metafiles]() -> int {
#if !IS_WINDOWS
    return mkdir(metafiles.c_str(), 0755);
#else
    return mkdir(metafiles.c_str());
#endif
  }();
  if (status != 0 && errno != EEXIST) {
    // Attention: errno may get changed by syscalls or lib functions.
    // Saving before printing is a conventional way of using errno.
    int errsv = errno;
    std::cerr << "error: cannot mkdir meta in outdir. errno = " << errsv
              << std::endl;
    exit(EXIT_FAILURE);
  }

  TRACE(MAIN, 2, "Verify-none mode: %s",
        args.redex_options.verify_none_enabled ? "Yes" : "No");
  TRACE(MAIN, 2, "Art build: %s",
        args.redex_options.is_art_build ? "Yes" : "No");
  TRACE(MAIN, 2, "Enable InstrumentPass: %s",
        args.redex_options.instrument_pass_enabled ? "Yes" : "No");

  return args;
}

Json::Value get_stats(const dex_stats_t& stats) {
  Json::Value val;
  val["num_types"] = stats.num_types;
  val["num_type_lists"] = stats.num_type_lists;
  val["num_classes"] = stats.num_classes;
  val["num_methods"] = stats.num_methods;
  val["num_method_refs"] = stats.num_method_refs;
  val["num_fields"] = stats.num_fields;
  val["num_field_refs"] = stats.num_field_refs;
  val["num_strings"] = stats.num_strings;
  val["num_protos"] = stats.num_protos;
  val["num_static_values"] = stats.num_static_values;
  val["num_annotations"] = stats.num_annotations;
  val["num_bytes"] = stats.num_bytes;
  val["num_instructions"] = stats.num_instructions;
  val["num_tries"] = stats.num_tries;

  val["num_unique_types"] = stats.num_unique_types;
  val["num_unique_protos"] = stats.num_unique_protos;
  val["num_unique_strings"] = stats.num_unique_strings;
  val["num_unique_method_refs"] = stats.num_unique_method_refs;
  val["num_unique_field_refs"] = stats.num_unique_field_refs;

  val["types_total_size"] = stats.types_total_size;
  val["protos_total_size"] = stats.protos_total_size;
  val["strings_total_size"] = stats.strings_total_size;
  val["method_refs_total_size"] = stats.method_refs_total_size;
  val["field_refs_total_size"] = stats.field_refs_total_size;

  val["num_dbg_items"] = stats.num_dbg_items;
  val["dbg_total_size"] = stats.dbg_total_size;

  val["instruction_bytes"] = stats.instruction_bytes;

  val["header_item_count"] = stats.header_item_count;
  val["header_item_bytes"] = stats.header_item_bytes;
  val["string_id_count"] = stats.string_id_count;
  val["string_id_bytes"] = stats.string_id_bytes;
  val["type_id_count"] = stats.type_id_count;
  val["type_id_bytes"] = stats.type_id_bytes;
  val["proto_id_count"] = stats.proto_id_count;
  val["proto_id_bytes"] = stats.proto_id_bytes;
  val["field_id_count"] = stats.field_id_count;
  val["field_id_bytes"] = stats.field_id_bytes;
  val["method_id_count"] = stats.method_id_count;
  val["method_id_bytes"] = stats.method_id_bytes;
  val["class_def_count"] = stats.class_def_count;
  val["class_def_bytes"] = stats.class_def_bytes;
  val["call_site_id_count"] = stats.call_site_id_count;
  val["call_site_id_bytes"] = stats.call_site_id_bytes;
  val["method_handle_count"] = stats.method_handle_count;
  val["method_handle_bytes"] = stats.method_handle_bytes;
  val["map_list_count"] = stats.map_list_count;
  val["map_list_bytes"] = stats.map_list_bytes;
  val["type_list_count"] = stats.type_list_count;
  val["type_list_bytes"] = stats.type_list_bytes;
  val["annotation_set_ref_list_count"] = stats.annotation_set_ref_list_count;
  val["annotation_set_ref_list_bytes"] = stats.annotation_set_ref_list_bytes;
  val["annotation_set_count"] = stats.annotation_set_count;
  val["annotation_set_bytes"] = stats.annotation_set_bytes;
  val["class_data_count"] = stats.class_data_count;
  val["class_data_bytes"] = stats.class_data_bytes;
  val["code_count"] = stats.code_count;
  val["code_bytes"] = stats.code_bytes;
  val["string_data_count"] = stats.string_data_count;
  val["string_data_bytes"] = stats.string_data_bytes;
  val["debug_info_count"] = stats.debug_info_count;
  val["debug_info_bytes"] = stats.debug_info_bytes;
  val["annotation_count"] = stats.annotation_count;
  val["annotation_bytes"] = stats.annotation_bytes;
  val["encoded_array_count"] = stats.encoded_array_count;
  val["encoded_array_bytes"] = stats.encoded_array_bytes;
  val["annotations_directory_count"] = stats.annotations_directory_count;
  val["annotations_directory_bytes"] = stats.annotations_directory_bytes;

  return val;
}

Json::Value get_pass_stats(const PassManager& mgr) {
  Json::Value all(Json::ValueType::objectValue);
  for (const auto& pass_info : mgr.get_pass_info()) {
    if (pass_info.metrics.empty()) {
      continue;
    }
    Json::Value pass;
    for (const auto& pass_metric : pass_info.metrics) {
      pass[pass_metric.first] = (Json::Int64)pass_metric.second;
    }
    all[pass_info.name] = pass;
  }
  return all;
}

Json::Value get_pass_hashes(const PassManager& mgr) {
  Json::Value all(Json::ValueType::objectValue);
  auto initial_hash = mgr.get_initial_hash();
  if (initial_hash) {
    all["(initial)-positions"] =
        hashing::hash_to_string(initial_hash->positions_hash);
    all["(initial)-registers"] =
        hashing::hash_to_string(initial_hash->registers_hash);
    all["(initial)-code"] = hashing::hash_to_string(initial_hash->code_hash);
    all["(initial)-signature"] =
        hashing::hash_to_string(initial_hash->signature_hash);
  }
  for (const auto& pass_info : mgr.get_pass_info()) {
    auto hash = pass_info.hash;
    if (hash) {
      all[pass_info.name + "-positions"] =
          hashing::hash_to_string(hash->positions_hash);
      all[pass_info.name + "-registers"] =
          hashing::hash_to_string(hash->registers_hash);
      all[pass_info.name + "-code"] = hashing::hash_to_string(hash->code_hash);
      all[pass_info.name + "-signature"] =
          hashing::hash_to_string(hash->signature_hash);
    }
  }
  return all;
}

Json::Value get_lowering_stats(const instruction_lowering::Stats& stats) {
  using namespace instruction_lowering;

  Json::Value obj(Json::ValueType::objectValue);
  obj["num_2addr_instructions"] = Json::UInt(stats.to_2addr);
  obj["num_move_added_for_check_cast"] = Json::UInt(stats.move_for_check_cast);

  if (!stats.sparse_switches.data.empty()) {
    // Some statistics.
    Json::Value sparse_switches(Json::ValueType::objectValue);
    sparse_switches["min"] =
        Json::UInt(stats.sparse_switches.data.begin()->first);
    sparse_switches["max"] =
        Json::UInt(stats.sparse_switches.data.rbegin()->first);
    sparse_switches["avg100"] = Json::UInt([&]() {
      size_t cnt = 0;
      size_t sum = 0;
      for (auto& p : stats.sparse_switches.data) {
        cnt += p.second.all;
        sum += p.second.all * p.first;
      }
      return sum * 100 / cnt;
    }());

    auto span = stats.sparse_switches.data.rbegin()->first -
                stats.sparse_switches.data.begin()->first;
    constexpr size_t kBuckets = 10;
    if (span > kBuckets) {
      const auto first = stats.sparse_switches.data.begin()->first;
      const auto last = stats.sparse_switches.data.rbegin()->first;

      auto it = stats.sparse_switches.data.begin();
      auto end = stats.sparse_switches.data.end();
      Json::Value buckets(Json::ValueType::objectValue);

      auto per_bucket = span / kBuckets;
      size_t cur_bucket_start = it->first;
      for (size_t i = 0; i != kBuckets; ++i) {
        auto start_size = first + i * per_bucket;
        auto end_size = first + (i + 1) * per_bucket;
        if (i == kBuckets - 1) {
          end_size = last + 1;
        }
        redex_assert(it->first >= start_size);
        redex_assert(end_size > start_size);

        Stats::SparseSwitches::Data tmp{};
        for (; it != end && it->first < end_size; ++it) {
          tmp += it->second;
        }

        if (tmp.all != 0) {
          Json::Value bucket(Json::ValueType::objectValue);
          bucket["all"] = Json::UInt(tmp.all);
          bucket["in_hot_methods"] = Json::UInt(tmp.in_hot_methods);
          bucket["min"] = Json::UInt(start_size);
          bucket["max"] = Json::UInt(end_size);
          buckets[std::to_string(i)] = bucket;
        }
      }
      redex_assert(it == end);
      sparse_switches["buckets"] = buckets;
    }

    obj["sparse_switches"] = sparse_switches;
  }

  return obj;
}

Json::Value get_position_stats(const PositionMapper* pos_mapper) {
  Json::Value obj(Json::ValueType::objectValue);
  obj["num_positions"] = Json::UInt(pos_mapper->size());
  return obj;
}

Json::Value get_detailed_stats(const std::vector<dex_stats_t>& dexes_stats) {
  Json::Value dexes;
  int i = 0;
  for (const dex_stats_t& stats : dexes_stats) {
    dexes[i++] = get_stats(stats);
  }
  return dexes;
}

Json::Value get_times(double cpu_time_s) {
  Json::Value list(Json::arrayValue);
  for (const auto& t : Timer::get_times()) {
    Json::Value element;
    element[t.first] = std::round(t.second * 10) / 10.0;
    list.append(element);
  }
  {
    Json::Value cpu_element;
    cpu_element["cpu_time"] = std::round(cpu_time_s * 10) / 10.0;
    list.append(cpu_element);
  }
  {
    Json::Value thread_pool_element;
    thread_pool_element["thread_pool_size"] =
        redex_thread_pool::ThreadPool::get_instance()->size() * 1.0;
    list.append(thread_pool_element);
  }
  return list;
}

Json::Value get_input_stats(const dex_stats_t& stats,
                            const std::vector<dex_stats_t>& dexes_stats) {
  Json::Value d;
  d["total_stats"] = get_stats(stats);
  d["dexes_stats"] = get_detailed_stats(dexes_stats);
  return d;
}

Json::Value get_output_dexes_stats(
    const std::vector<std::pair<std::string, enhanced_dex_stats_t>>&
        dexes_stats) {
  Json::Value dexes;
  int i = 0;
  for (const auto& [store_name, stats] : dexes_stats) {
    dexes[i] = get_stats(stats);
    dexes[i]["store_name"] = store_name;
    ++i;
  }
  return dexes;
}

Json::Value get_output_stats(
    const dex_stats_t& stats,
    const std::vector<std::pair<std::string, enhanced_dex_stats_t>>&
        dexes_stats,
    const PassManager& mgr,
    const instruction_lowering::Stats& instruction_lowering_stats,
    const PositionMapper* pos_mapper) {
  Json::Value d;
  d["total_stats"] = get_stats(stats);
  d["dexes_stats"] = get_output_dexes_stats(dexes_stats);
  d["pass_stats"] = get_pass_stats(mgr);
  d["pass_hashes"] = get_pass_hashes(mgr);
  d["lowering_stats"] = get_lowering_stats(instruction_lowering_stats);
  d["position_stats"] = get_position_stats(pos_mapper);
  return d;
}

Json::Value get_threads_stats() {
  Json::Value d;
  d["used"] = (Json::UInt64)redex_parallel::default_num_threads();
  d["hardware"] = (Json::UInt64)boost::thread::hardware_concurrency();
  d["physical"] = (Json::UInt64)boost::thread::physical_concurrency();
  return d;
}

void write_debug_line_mapping(
    const std::string& debug_line_map_filename,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id,
    const std::unordered_map<DexCode*, std::vector<DebugLineItem>>&
        code_debug_lines,
    DexStoresVector& stores,
    const std::vector<DexMethod*>& needs_debug_line_mapping) {
  /*
   * Binary file format:
   * magic number 0xfaceb000 (4 byte)
   * version number (4 byte)
   * number (m) of methods that has debug line info (4 byte)
   * a list (m elements) of:
   *   [ encoded method-id (8 byte), method debug info byte offset (4 byte),
   *     method debug info byte size (4 byte) ]
   *
   * a list (m elements) of :
   *   encoded method-id (8 byte)
   *   a list (n elements) of:
   *     [ memory offset (4 byte), line number (4 byte) ]
   */
  size_t bit_32_size = sizeof(uint32_t);
  size_t bit_64_size = sizeof(uint64_t);
  uint32_t num_method = code_debug_lines.size();
  int offset = num_method + 2;
  // Start of debug line info information would be after all of
  // method-id => offset info, so set the start of offset to be after that.
  int binary_offset =
      3 * bit_32_size + (bit_64_size + 2 * bit_32_size) * num_method;
  std::ofstream ofs(debug_line_map_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  uint32_t magic = 0xfaceb000; // serves as endianess check
  ofs.write((const char*)&magic, bit_32_size);
  uint32_t version = 1;
  ofs.write((const char*)&version, bit_32_size);
  ofs.write((const char*)&num_method, bit_32_size);
  std::ostringstream line_out;

  auto scope = build_class_scope(stores);
  std::vector<DexMethod*> all_methods(std::begin(needs_debug_line_mapping),
                                      std::end(needs_debug_line_mapping));
  walk::methods(scope,
                [&](DexMethod* method) { all_methods.push_back(method); });
  std::stable_sort(all_methods.begin(), all_methods.end(), compare_dexmethods);

  for (auto* method : all_methods) {
    auto dex_code = method->get_dex_code();
    if (dex_code == nullptr ||
        code_debug_lines.find(dex_code) == code_debug_lines.end()) {
      continue;
    }

    uint64_t method_id = method_to_id.at(method);
    // write method id => offset info for binary file
    ofs.write((const char*)&method_id, bit_64_size);
    ofs.write((const char*)&binary_offset, bit_32_size);

    auto debug_lines = code_debug_lines.at(dex_code);
    uint32_t num_line_info = debug_lines.size();
    offset = offset + 1 + num_line_info;
    uint32_t info_section_size = bit_64_size + num_line_info * 2 * bit_32_size;
    ofs.write((const char*)&info_section_size, bit_32_size);
    binary_offset = binary_offset + info_section_size;

    // Generate debug line info for binary file.
    line_out.write((const char*)&method_id, bit_64_size);
    for (auto it = debug_lines.begin(); it != debug_lines.end(); ++it) {
      line_out.write((const char*)&it->offset, bit_32_size);
      line_out.write((const char*)&it->line, bit_32_size);
    }
  }
  ofs << line_out.str();
}

std::string get_dex_magic(std::vector<std::string>& dex_files) {
  always_assert_log(!dex_files.empty(), "APK contains no dex file\n");
  // Get dex magic from the first dex file since all dex magic
  // should be consistent within one APK.
  return load_dex_magic_from_dex(
      DexLocation::make_location("dex", dex_files[0]));
}

void dump_keep_reasons(const ConfigFiles& conf,
                       const Arguments& args,
                       const DexStoresVector& stores) {
  if (!args.config.get("dump_keep_reasons", false).asBool()) {
    return;
  }

  std::ofstream ofs(conf.metafile("redex-keep-reasons.txt"));

  auto scope = build_class_scope(stores);
  for (const auto* cls : scope) {
    auto has_keep_reasons = [cls]() {
      if (!cls->rstate.keep_reasons().empty()) {
        return true;
      }
      for (auto* m : cls->get_all_methods()) {
        if (!m->rstate.keep_reasons().empty()) {
          return true;
        }
      }
      for (auto* f : cls->get_all_fields()) {
        if (!f->rstate.keep_reasons().empty()) {
          return true;
        }
      }
      return false;
    };
    if (!has_keep_reasons()) {
      continue;
    }
    auto print_keep_reasons = [&ofs](const auto& reasons, const char* indent) {
      for (const auto* r : reasons) {
        ofs << indent << "* " << *r << "\n";
      }
    };
    ofs << "Class: " << show_deobfuscated(cls) << "\n";
    print_keep_reasons(cls->rstate.keep_reasons(), " ");

    auto print_list = [&ofs, &print_keep_reasons](const auto& c,
                                                  const char* name) {
      for (const auto* member : c) {
        if (member->rstate.keep_reasons().empty()) {
          continue; // Skip stuff w/o reasons.
        }
        ofs << " " << name << ": " << show_deobfuscated(member) << "\n";
        print_keep_reasons(member->rstate.keep_reasons(), "  ");
      }
    };
    print_list(cls->get_all_fields(), "Field");
    print_list(cls->get_all_methods(), "Method");
  }
}

void process_proguard_rules(ConfigFiles& conf,
                            Scope& scope,
                            Scope& external_classes,
                            keep_rules::ProguardConfiguration& pg_config) {
  bool keep_all_annotation_classes;
  conf.get_json_config().get("keep_all_annotation_classes", true,
                             keep_all_annotation_classes);

  bool record_used_rules;
  conf.get_json_config().get("record_accessed_rules", true, record_used_rules);

  bool unused_rule_abort;
  conf.get_json_config().get("unused_keep_rule_abort", false,
                             unused_rule_abort);
  auto proguard_rule_recorder =
      process_proguard_rules(conf.get_proguard_map(), scope, external_classes,
                             pg_config, keep_all_annotation_classes);
  if (record_used_rules) {
    proguard_rule_recorder.record_accessed_rules(
        conf.metafile("redex-used-proguard-rules.txt"),
        conf.metafile("redex-unused-proguard-rules.txt"));
  }
  if (unused_rule_abort) {
    std::vector<std::string> unused_out;
    for (const keep_rules::KeepSpec* keep_rule :
         proguard_rule_recorder.unused_keep_rules) {
      unused_out.push_back(keep_rules::show_keep(*keep_rule));
    }
    // Make output deterministic
    if (!unused_out.empty()) {
      std::sort(unused_out.begin(), unused_out.end());
      always_assert_log(false, "%s",
                        [&]() {
                          std::string tmp;
                          for (const auto& s : unused_out) {
                            tmp += s;
                            tmp += " not used\n";
                          }
                          return tmp;
                        }()
                            .c_str());
    }
  }
}

void load_library_jars(Arguments& args,
                       Scope& external_classes,
                       const std::set<std::string>& library_jars,
                       const std::string& base_dir) {
  args.entry_data["jars"] = Json::arrayValue;
  if (library_jars.empty()) {
    return;
  }

  auto load = [&](const auto& allowed_fn) {
    for (const auto& library_jar : library_jars) {
      TRACE(MAIN, 1, "LIBRARY JAR: %s", library_jar.c_str());
      if (load_jar_file(DexLocation::make_location("", library_jar),
                        &external_classes, /*attr_hook=*/nullptr, allowed_fn)) {
        auto abs_path = boost::filesystem::absolute(library_jar);
        args.entry_data["jars"].append(abs_path.string());
        continue;
      }

      // Try again with the basedir
      std::string basedir_path = base_dir + "/" + library_jar;
      if (load_jar_file(DexLocation::make_location("", basedir_path),
                        /*classes=*/nullptr, /*attr_hook=*/nullptr,
                        allowed_fn)) {
        args.entry_data["jars"].append(basedir_path);
        continue;
      }

      std::cerr << "error: library jar could not be loaded: " << library_jar
                << std::endl;
      _exit(EXIT_FAILURE);
    }
  };

  // We cannot use GlobalConfig here, it is too early.
  JarLoaderConfig jar_conf{};
  if (args.config.isMember("jar_loader")) {
    jar_conf.parse_config(
        JsonWrapper(args.config.get("jar_loader", Json::nullValue)));
  }

  Timer t("Load library jars");

  if (jar_conf.legacy_mode) {
    load(jar_loader::default_duplicate_allow_fn);
  } else {
    load([&](auto* c, auto& jar_name [[maybe_unused]]) {
      auto sv = c->get_name()->str();
      for (const auto& prefix : jar_conf.allowed_prefixes) {
        if (sv.substr(0, prefix.size()) == prefix) {
          return true;
        }
      }
      return false;
    });
  }
}

/**
 * Pre processing steps: load dex and configurations
 */
void redex_frontend(ConfigFiles& conf, /* input */
                    Arguments& args, /* inout */
                    keep_rules::ProguardConfiguration& pg_config,
                    DexStoresVector& stores,
                    Json::Value& stats) {
  Timer redex_frontend_timer("Redex_frontend");

  g_redex->load_pointers_cache();

  keep_rules::proguard_parser::Stats parser_stats{};
  for (const auto& pg_config_path : args.proguard_config_paths) {
    Timer time_pg_parsing("Parsed ProGuard config file");
    parser_stats +=
        keep_rules::proguard_parser::parse_file(pg_config_path, &pg_config);
  }

  size_t blocklisted_rules{0};
  {
    // We cannot use GlobalConfig here, it is too early.
    ProguardConfig pg_conf{};
    if (conf.get_json_config().contains("proguard")) {
      pg_conf.parse_config(
          JsonWrapper(conf.get_json_config().get("proguard", Json::Value())));
    }
    for (const auto& block_rules : pg_conf.blocklist) {
      blocklisted_rules +=
          keep_rules::proguard_parser::remove_blocklisted_rules(block_rules,
                                                                &pg_config);
    }
    if (!pg_conf.disable_default_blocklist) {
      blocklisted_rules +=
          keep_rules::proguard_parser::remove_default_blocklisted_rules(
              &pg_config);
    }

    always_assert(!pg_conf.fail_on_unknown_commands ||
                  parser_stats.unknown_commands == 0);
  }

  // WARNING: No further modifications of pg_config should happen after this
  // call
  // TODO: T148153725: [redex] Better encapsulate ProguardConfiguration
  // construction
  keep_rules::proguard_parser::identify_blanket_native_rules(&pg_config);

  auto ignore_no_keep_rules =
      args.config.get("ignore_no_keep_rules", false).asBool();
  if (pg_config.keep_rules.empty() && !ignore_no_keep_rules) {
    std::cerr << "error: No ProGuard keep rules provided. Redex optimizations "
                 "will not preserve semantics without accurate keep rules."
              << std::endl;
    exit(EXIT_FAILURE);
  }

  {
    Json::Value d;
    using u64 = Json::Value::UInt64;
    d["parse_errors"] = (u64)parser_stats.parse_errors;
    d["unknown_tokens"] = (u64)parser_stats.unknown_tokens;
    d["unimplemented"] = (u64)parser_stats.unimplemented;
    d["unknown_commands"] = (u64)parser_stats.unknown_commands;
    d["ok"] = (u64)(pg_config.ok ? 1 : 0);
    d["blocklisted_rules"] = (u64)blocklisted_rules;
    d["blanket_native_rules"] = (u64)(std::distance(
        pg_config.keep_rules_native_begin.value_or(pg_config.keep_rules.end()),
        pg_config.keep_rules.end()));
    stats["proguard"] = d;
  }

  const auto& pg_libs = pg_config.libraryjars;
  args.jar_paths.insert(pg_libs.begin(), pg_libs.end());

  std::set<std::string> library_jars;
  for (const auto& jar_path : args.jar_paths) {
    std::istringstream jar_stream(jar_path);
    std::string dependent_jar_path;
    constexpr char kDelim =
#if IS_WINDOWS
        ';';
#else
        ':';
#endif
    while (std::getline(jar_stream, dependent_jar_path, kDelim)) {
      TRACE(MAIN,
            2,
            "Dependent JAR specified on command-line: %s",
            dependent_jar_path.c_str());
      library_jars.emplace(dependent_jar_path);
    }
  }

  DexStore root_store("classes");
  // Only set dex magic to root DexStore since all dex magic
  // should be consistent within one APK.
  root_store.set_dex_magic(get_dex_magic(args.dex_files));
  stores.emplace_back(std::move(root_store));

  const JsonWrapper& json_config = conf.get_json_config();
  dup_classes::read_dup_class_allowlist(json_config);

  run_rethrow_first_aggregate([&]() {
    Timer t("Load classes from dexes");
    dex_stats_t input_totals;
    std::vector<dex_stats_t> input_dexes_stats;
    redex::load_classes_from_dexes_and_metadata(
        args.dex_files, stores, input_totals, input_dexes_stats);
    stats["input_stats"] = get_input_stats(input_totals, input_dexes_stats);
  });

  Scope external_classes;
  load_library_jars(args, external_classes, library_jars,
                    pg_config.basedirectory);

  {
    Timer t("Deobfuscating dex elements");
    for (auto& store : stores) {
      apply_deobfuscated_names(store.get_dexen(), conf.get_proguard_map());
    }
  }
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("No Optimizations Rules");
    // this will change rstate of methods
    keep_rules::process_no_optimizations_rules(
        conf.get_no_optimizations_annos(),
        conf.get_no_optimizations_blocklist(), scope);
  }
  {
    Timer t("Initializing reachable classes");
    // init reachable will change rstate of classes, methods and fields
    init_reachable_classes(scope, ReachableClassesConfig(json_config));
  }
  {
    Timer t("Processing proguard rules");
    process_proguard_rules(conf, scope, external_classes, pg_config);
  }

  TRACE(NATIVE, 2, "Blanket native classes: %zu",
        g_redex->blanket_native_root_classes.size());
  TRACE(NATIVE, 2, "Blanket native methods: %zu",
        g_redex->blanket_native_root_methods.size());

  if (keep_reason::Reason::record_keep_reasons()) {
    dump_keep_reasons(conf, args, stores);
  }
}

// Performa check for resources that must exist for app to behave correctly,
// crash the build if they fail to present.
void check_required_resources(ConfigFiles& conf, bool pre_run) {
  std::vector<std::string> check_required_resources;
  conf.get_json_config().get("check_required_resources", {},
                             check_required_resources);
  if (check_required_resources.empty()) {
    return;
  }

  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);
  TRACE(MAIN, 1, "Validating resources.");
  auto resources = create_resource_reader(apk_dir);
  auto res_table = resources->load_res_table();
  for (const auto& required_resource : check_required_resources) {
    always_assert_log(res_table->name_to_ids.count(required_resource) != 0,
                      "Required resource %s does not exist in %s apk",
                      required_resource.c_str(),
                      pre_run ? "input" : "final");
  }
}

// Performa final wave of cleanup (i.e. garbage collect unreferenced strings,
// etc) so that this only needs to happen once and not after every resource
// modification.
void finalize_resource_table(ConfigFiles& conf) {
  if (!conf.finalize_resource_table()) {
    return;
  }
  const auto& json = conf.get_json_config();
  if (json.get("after_pass_size", false)) {
    return;
  }

  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);
  if (apk_dir.empty()) {
    return;
  }
  TRACE(MAIN, 1, "Finalizing resource table.");
  auto resources = create_resource_reader(apk_dir);
  auto res_table = resources->load_res_table();
  auto global_resources_config =
      conf.get_global_config().get_config_by_name<ResourceConfig>("resources");
  res_table->finalize_resource_table(*global_resources_config);
}

/**
 * Post processing steps: write dex and collect stats
 */
void redex_backend(ConfigFiles& conf,
                   PassManager& manager,
                   DexStoresVector& stores,
                   Json::Value& stats) {
  Timer redex_backend_timer("Redex_backend");
  const RedexOptions& redex_options = manager.get_redex_options();
  const auto& output_dir = conf.get_outdir();

  finalize_resource_table(conf);
  check_required_resources(conf, false);

  instruction_lowering::Stats instruction_lowering_stats;
  {
    bool lower_with_cfg = true;
    conf.get_json_config().get("lower_with_cfg", true, lower_with_cfg);
    Timer t("Instruction lowering");
    instruction_lowering_stats =
        instruction_lowering::run(stores, lower_with_cfg, &conf);
  }

  sanitizers::lsan_do_recoverable_leak_check();

  TRACE(MAIN, 1, "Writing out new DexClasses...");
  const JsonWrapper& json_config = conf.get_json_config();

  LocatorIndex* locator_index = nullptr;
  if (json_config.get("emit_locator_strings", false)) {
    TRACE(LOC, 1,
          "Will emit class-locator strings for classloader optimization");
    locator_index = new LocatorIndex(make_locator_index(stores));
  }

  enhanced_dex_stats_t output_totals;
  std::vector<std::pair<std::string, enhanced_dex_stats_t>> output_dexes_stats;

  const std::string& line_number_map_filename = conf.metafile(LINE_NUMBER_MAP);
  const std::string& debug_line_map_filename = conf.metafile(DEBUG_LINE_MAP);
  const std::string& iodi_metadata_filename = conf.metafile(IODI_METADATA);

  auto dik = redex_options.debug_info_kind;
  bool needs_addresses = dik == DebugInfoKind::NoPositions || is_iodi(dik);

  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make(
      dik == DebugInfoKind::NoCustomSymbolication ? ""
                                                  : line_number_map_filename));
  std::unordered_map<DexMethod*, uint64_t> method_to_id;
  std::unordered_map<DexCode*, std::vector<DebugLineItem>> code_debug_lines;

  auto iodi_metadata = [&]() {
    auto val = conf.get_json_config().get("iodi_layer_mode", Json::Value());
    return IODIMetadata(redex_options.min_sdk,
                        val.isNull()
                            ? IODIMetadata::IODILayerMode::kFull
                            : IODIMetadata::parseLayerMode(val.asString()));
  }();

  std::set<uint32_t> signatures;
  std::unique_ptr<PostLowering> post_lowering =
      redex_options.post_lowering ? PostLowering::create() : nullptr;

  const bool mem_stats_enabled =
      traceEnabled(STATS, 1) || conf.get_json_config().get("mem_stats", true);
  const bool reset_hwm = conf.get_json_config().get("mem_stats_per_pass", true);

  if (is_iodi(dik)) {
    Timer t("Compute initial IODI metadata");
    ScopedMemStats iodi_mem_stats{mem_stats_enabled, reset_hwm};
    iodi_metadata.mark_methods(stores,
                               dik == DebugInfoKind::InstructionOffsetsLayered);
    iodi_mem_stats.trace_log("Compute initial IODI metadata");
  }

  const auto& dex_output_config =
      *conf.get_global_config().get_config_by_name<DexOutputConfig>(
          "dex_output");

  auto string_sort_mode = get_string_sort_mode(conf);

  bool should_preserve_input_dexes =
      conf.get_json_config().get("preserve_input_dexes", false);
  if (should_preserve_input_dexes) {
    always_assert_log(
        !post_lowering,
        "post lowering should be off when preserving input dex option is on");
    TRACE(MAIN, 1, "Skipping writing output dexes as configured");
  } else {
    redex_assert(!stores.empty());
    const auto& dex_magic = stores[0].get_dex_magic();
    auto min_sdk = manager.get_redex_options().min_sdk;
    ScopedMemStats wod_mem_stats{mem_stats_enabled, reset_hwm};
    for (size_t store_number = 0; store_number < stores.size();
         ++store_number) {
      auto& store = stores[store_number];
      const auto& store_name = store.get_name();
      auto code_sort_mode = get_code_sort_mode(conf, store_name);
      Timer t("Writing optimized dexes");
      for (size_t i = 0; i < store.get_dexen().size(); i++) {
        DexClasses* classes = &store.get_dexen()[i];
        auto gtypes = std::make_shared<GatheredTypes>(classes);

        if (post_lowering) {
          post_lowering->load_dex_indexes(conf, min_sdk, classes, *gtypes,
                                          store_name, i);
        }

        auto this_dex_stats = write_classes_to_dex(
            redex::get_dex_output_name(output_dir, store, i),
            classes,
            gtypes,
            locator_index,
            store_number,
            &store_name,
            i,
            conf,
            pos_mapper.get(),
            redex_options.debug_info_kind,
            needs_addresses ? &method_to_id : nullptr,
            needs_addresses ? &code_debug_lines : nullptr,
            is_iodi(dik) ? &iodi_metadata : nullptr,
            dex_magic,
            dex_output_config,
            min_sdk,
            code_sort_mode,
            string_sort_mode);

        output_totals += this_dex_stats;
        // Remove class sizes here to free up memory.
        this_dex_stats.class_size.clear();
        signatures.insert(
            *reinterpret_cast<uint32_t*>(this_dex_stats.signature));
        output_dexes_stats.push_back(
            std::make_pair(store.get_name(), std::move(this_dex_stats)));
      }
    }
    wod_mem_stats.trace_log("Writing optimized dexes");
  }

  sanitizers::lsan_do_recoverable_leak_check();

  std::vector<DexMethod*> needs_debug_line_mapping;
  if (post_lowering) {
    post_lowering->run(stores);
    post_lowering->finalize(manager.asset_manager());
  }

  {
    Timer t("Writing opt decisions data");
    const Json::Value& opt_decisions_args = json_config["opt_decisions"];
    if (opt_decisions_args.get("enable_logs", false).asBool()) {
      auto opt_decisions_output_path = conf.metafile(OPT_DECISIONS);
      auto opt_data =
          opt_metadata::OptDataMapper::get_instance().serialize_sql();
      {
        std::ofstream opt_data_out(opt_decisions_output_path);
        opt_data_out << opt_data;
      }
    }
  }

  {
    Timer t("Writing stats");
    auto method_move_map =
        conf.metafile(json_config.get("method_move_map", std::string()));
    if (needs_addresses) {
      Timer t2{"Writing debug line mapping"};
      write_debug_line_mapping(debug_line_map_filename, method_to_id,
                               code_debug_lines, stores,
                               needs_debug_line_mapping);
    }
    if (is_iodi(dik)) {
      Timer t2{"Writing IODI metadata"};
      iodi_metadata.write(iodi_metadata_filename, method_to_id);
    }
    {
      Timer t2{"Writing position map"};
      pos_mapper->write_map();
    }
    {
      Timer t2{"Collecting output stats"};
      stats["output_stats"] =
          get_output_stats(output_totals, output_dexes_stats, manager,
                           instruction_lowering_stats, pos_mapper.get());
    }
    print_warning_summary();

    if (dex_output_config.write_class_sizes) {
      Timer t2("Writing class sizes");
      // Sort for stability.
      std::vector<const DexClass*> keys;
      std::transform(output_totals.class_size.begin(),
                     output_totals.class_size.end(), std::back_inserter(keys),
                     [](const auto& p) { return p.first; });
      std::sort(keys.begin(), keys.end(), compare_dexclasses);
      std::ofstream ofs{conf.metafile("redex-class-sizes.csv")};
      for (auto* c : keys) {
        ofs << c->get_deobfuscated_name_or_empty() << ","
            << output_totals.class_size.at(c) << "\n";
      }
    }
  }
}

void dump_class_method_info_map(const std::string& file_path,
                                DexStoresVector& stores) {
  std::ofstream ofs(file_path, std::ofstream::out | std::ofstream::trunc);

  static const char* header =
      "# This map enumerates all class and method sizes and some properties.\n"
      "# To minimize the size, dex location strings are interned.\n"
      "# Class information is also interned.\n"
      "#\n"
      "# First column can be M, C, and I.\n"
      "# - C => Class index and information\n"
      "# - M => Method information\n"
      "# - I,DEXLOC => Dex location string index\n"
      "#\n"
      "# C,<index>,<obfuscated class name>,<deobfuscated class name>,\n"
      "#   <# of all methods>,<# of all virtual methods>,\n"
      "#   <dex location string index>\n"
      "# M,<class index>,<obfuscated method name>,<deobfuscated method name>,\n"
      "#   <size>,<virtual>,<external>,<concrete>\n"
      "# I,DEXLOC,<index>,<string>";
  ofs << header << std::endl;

  auto exclude_class_name = [&](const std::string& full_name) {
    const auto dot_pos = full_name.find('.');
    always_assert(dot_pos != std::string::npos);
    // Return excluding class name and "."
    return full_name.substr(dot_pos + 1);
  };

  auto print = [&](const int cls_idx, const DexMethod* method) {
    ofs << "M," << cls_idx << "," << exclude_class_name(show(method)) << ","
        << exclude_class_name(method->get_fully_deobfuscated_name()) << ","
        << (method->get_dex_code() ? method->get_dex_code()->size() : 0) << ","
        << method->is_virtual() << "," << method->is_external() << ","
        << method->is_concrete() << std::endl;
  };

  // Interning
  std::unordered_map<const DexClass*, int /*index*/> class_map;
  std::unordered_map<std::string /*location*/, int /*index*/> dexloc_map;

  walk::classes(build_class_scope(stores), [&](const DexClass* cls) {
    const auto& dexloc = cls->get_location()->get_file_name();
    if (!dexloc_map.count(dexloc)) {
      dexloc_map[dexloc] = dexloc_map.size();
      ofs << "I,DEXLOC," << dexloc_map[dexloc] << "," << dexloc << std::endl;
    }

    redex_assert(!class_map.count(cls));
    const int cls_idx = (class_map[cls] = class_map.size());
    ofs << "C," << cls_idx << "," << show(cls) << "," << show_deobfuscated(cls)
        << "," << (cls->get_dmethods().size() + cls->get_vmethods().size())
        << "," << cls->get_vmethods().size() << "," << dexloc_map[dexloc]
        << std::endl;

    for (auto dmethod : cls->get_dmethods()) {
      print(cls_idx, dmethod);
    }
    for (auto vmethod : cls->get_vmethods()) {
      print(cls_idx, vmethod);
    }
  });
}

void maybe_dump_jemalloc_profile(const char* env_name) {
  auto* dump_path = std::getenv(env_name);
  if (dump_path != nullptr) {
    jemalloc_util::dump(dump_path);
  }
}

void copy_proguard_stats(Json::Value& stats) {
  if (!stats.isMember("proguard")) {
    return;
  }
  if (!stats.isMember("output_stats")) {
    return;
  }
  auto& output = stats["output_stats"];
  if (!output.isMember("pass_stats")) {
    return;
  }
  auto& passes = output["pass_stats"];

  Json::Value* first_pass = nullptr;
  for (const auto& name : passes.getMemberNames()) {
    auto& pass = passes[name];
    if (first_pass == nullptr || pass["pass_order"].asUInt64() <
                                     (*first_pass)["pass_order"].asUInt64()) {
      first_pass = &pass;
    }
  }

  if (first_pass == nullptr) {
    return;
  }

  auto& pr = stats["proguard"];
  for (const auto& name : pr.getMemberNames()) {
    std::string compound_name = "proguard_" + name;
    (*first_pass)[compound_name] = pr[name];
  }
}

int check_pass_properties(const Arguments& args) {
  using namespace redex_properties;
  // Cannot parse GlobalConfig nor passes, as they may require binding
  // to dex elements. So this looks more complicated than necessary.

  ConfigFiles conf(args.config, args.out_dir);

  PassManagerConfig pmc;
  if (conf.get_json_config().contains("pass_manager")) {
    pmc.parse_config(JsonWrapper(conf.get_json_config().get(
        "pass_manager", (Json::Value)Json::nullValue)));
  }

  if (!pmc.check_pass_order_properties &&
      args.properties_check_allow_disabled) {
    std::cout << "Properties checks are disabled, skipping." << std::endl;
    return 0;
  }

  auto const& all_passes = PassRegistry::get().get_passes();
  auto props_manager =
      Manager(conf, PropertyCheckerRegistry::get().get_checkers());
  auto active_passes =
      PassManager::compute_activated_passes(all_passes, conf, &pmc);

  std::vector<std::pair<std::string, PropertyInteractions>> pass_interactions;
  for (const auto& [pass, _] : active_passes.activated_passes) {
    auto m = pass->get_property_interactions();
    for (auto it = m.begin(); it != m.end();) {
      auto&& [name, property_interaction] = *it;

      if (!props_manager.property_is_enabled(name)) {
        it = m.erase(it);
        continue;
      }

      always_assert_log(property_interaction.is_valid(),
                        "%s has an invalid property interaction for %s",
                        pass->name().c_str(), get_name(name));
      ++it;
    }
    pass_interactions.emplace_back(pass->name(), std::move(m));
  }
  auto failure = Manager::verify_pass_interactions(pass_interactions, conf);
  if (failure) {
    std::cerr << "Illegal pass order:\n" << *failure << std::endl;
    return 1;
  }
  return 0;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char* argv[]) {
  signal(SIGABRT, debug_backtrace_handler);
  signal(SIGINT, debug_backtrace_handler);
  signal(SIGSEGV, crash_backtrace_handler);
#if !IS_WINDOWS
  signal(SIGBUS, crash_backtrace_handler);
#endif

  // Only log one assert.
  block_multi_asserts(/*block=*/true);
  // For better stacks in abort dumps.
  set_abort_if_not_this_thread();

  // For Breadcrumbs issues, do not throw, do not print stack trace. Improves
  // error readability.
  redex_debug::set_exc_type_as_abort(RedexError::REJECTED_CODING_PATTERN);
  redex_debug::disable_stack_trace_for_exc_type(
      RedexError::REJECTED_CODING_PATTERN);

  // Input type check issues are a straight issue, not a Redex crash.
  redex_debug::set_exc_type_as_abort(RedexError::TYPE_CHECK_ERROR);
  redex_debug::disable_stack_trace_for_exc_type(RedexError::TYPE_CHECK_ERROR);

  auto maybe_global_profile =
      ScopedCommandProfiling::maybe_from_env("GLOBAL_", "global");

  redex_thread_pool::ThreadPool::create();

  ConcurrentContainerConcurrentDestructionScope
      concurrent_container_destruction_scope;

  std::string stats_output_path;
  Json::Value stats;
  double cpu_time_s;
  {
    Timer redex_all_main_timer("redex-all main()");

    g_redex = new RedexContext();

    // Currently there are two sources that specify the library jars:
    // 1. The jar_path argument, which may specify one library jar.
    // 2. The library_jars vector, which lists the library jars specified in
    //    the ProGuard configuration.
    // If -jarpath specified a library jar it is appended to the
    // library_jars vector so this vector can be used to iterate over
    // all the library jars regardless of whether they were specified
    // on the command line or ProGuard file.
    // TODO: Make the command line -jarpath option like a colon separated
    //       list of library JARS.
    Arguments args = parse_args(argc, argv);

    if (args.properties_check) {
      return check_pass_properties(args);
    }

    keep_reason::Reason::set_record_keep_reasons(
        args.config.get("record_keep_reasons", false).asBool());

    // For convenience.
    g_redex->instrument_mode = args.redex_options.instrument_pass_enabled;
    if (g_redex->instrument_mode) {
      IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
    }
    {
      auto consecutive_val =
          args.config.get("sb_consecutive_style", Json::nullValue);
      if (consecutive_val.isString()) {
        auto str = consecutive_val.asString();
        IRList::CONSECUTIVE_STYLE = [&]() {
          if (str == "drop") {
            return IRList::ConsecutiveStyle::kDrop;
          } else if (str == "chain") {
            return IRList::ConsecutiveStyle::kChain;
          } else if (str == "max") {
            return IRList::ConsecutiveStyle::kMax;
          } else {
            not_reached_log("Unknown sb_consecutive_style %s", str.c_str());
          }
        }();
      }
    }

    slow_invariants_debug =
        args.config.get("slow_invariants_debug", false).asBool();
    cfg::ControlFlowGraph::DEBUG =
        cfg::ControlFlowGraph::DEBUG || slow_invariants_debug;
    if (slow_invariants_debug) {
      std::cerr << "Slow invariants enabled." << std::endl;
    }

    auto pg_config = std::make_unique<keep_rules::ProguardConfiguration>();
    DexStoresVector stores;
    ConfigFiles conf(args.config, args.out_dir);

    std::string apk_dir;
    conf.get_json_config().get("apk_dir", "", apk_dir);
    auto resources = create_resource_reader(apk_dir);
    boost::optional<int32_t> maybe_sdk = resources->get_min_sdk();
    if (maybe_sdk != boost::none) {
      TRACE(MAIN, 2, "parsed minSdkVersion = %d", *maybe_sdk);
      args.redex_options.min_sdk = *maybe_sdk;
    }

    {
      auto profile_frontend =
          ScopedCommandProfiling::maybe_from_env("FRONTEND_", "frontend");
      redex_frontend(conf, args, *pg_config, stores, stats);
      conf.parse_global_config();
      maybe_dump_jemalloc_profile("MALLOC_PROFILE_DUMP_FRONTEND");
    }

    check_required_resources(conf, true);

    auto const& passes = PassRegistry::get().get_passes();
    auto props_manager = redex_properties::Manager(
        conf, redex_properties::PropertyCheckerRegistry::get().get_checkers());
    PassManager manager(passes, std::move(pg_config), conf, args.redex_options,
                        &props_manager);

    {
      Timer t("Running optimization passes");
      manager.run_passes(stores, conf);
      maybe_dump_jemalloc_profile("MALLOC_PROFILE_DUMP_AFTER_ALL_PASSES");
    }

    if (args.stop_pass_idx == boost::none) {
      // Call redex_backend by default
      auto profile_backend =
          ScopedCommandProfiling::maybe_from_env("BACKEND_", "backend");
      redex_backend(conf, manager, stores, stats);
      if (args.config.get("emit_class_method_info_map", false).asBool()) {
        dump_class_method_info_map(conf.metafile(CLASS_METHOD_INFO_MAP),
                                   stores);
      }
    } else {
      redex::write_all_intermediate(conf, args.out_dir, args.redex_options,
                                    stores, args.entry_data);
    }
    maybe_dump_jemalloc_profile("MALLOC_PROFILE_DUMP_BACKEND");

    stats_output_path = conf.metafile(
        args.config.get("stats_output", "redex-stats.txt").asString());

    {
      Timer t("Freeing global memory");
      delete g_redex;
    }
    cpu_time_s = ((double)std::clock()) / CLOCKS_PER_SEC;
  }
  // now that all the timers are done running, we can collect the data
  stats["output_stats"]["time_stats"] = get_times(cpu_time_s);

  auto vm_stats = get_mem_stats();
  stats["output_stats"]["mem_stats"]["vm_peak"] =
      (Json::UInt64)vm_stats.vm_peak;
  stats["output_stats"]["mem_stats"]["vm_hwm"] = (Json::UInt64)vm_stats.vm_hwm;

  stats["output_stats"]["threads"] = get_threads_stats();

  stats["output_stats"]["build_cfg_counter"] = (Json::UInt64)build_cfg_counter;
  // For the time being, copy proguard stats, if any, to the first pass.
  copy_proguard_stats(stats);

  {
    std::ofstream out(stats_output_path);
    out << stats;
  }

  TRACE(MAIN, 1, "Done.");
  if (traceEnabled(MAIN, 1) || traceEnabled(STATS, 1)) {
    TRACE(STATS, 0, "Memory stats: VmPeak=%s VmHWM=%s",
          pretty_bytes(vm_stats.vm_peak).c_str(),
          pretty_bytes(vm_stats.vm_hwm).c_str());
  }

  redex_thread_pool::ThreadPool::destroy();

  malloc_debug::set_shutdown();

  return 0;
}
