/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cinttypes>
#include <cstring>
#include <fstream>
#include <iostream>
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

#include "ABExperimentContext.h"
#include "CommentFilter.h"
#include "ControlFlow.h" // To set DEBUG.
#include "Debug.h"
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
#include "Macros.h"
#include "MonitorCount.h"
#include "NoOptimizationsMatcher.h"
#include "OptData.h"
#include "PassRegistry.h"
#include "PostLowering.h"
#include "ProguardConfiguration.h" // New ProGuard configuration
#include "ProguardMatcher.h"
#include "ProguardParser.h" // New ProGuard Parser
#include "ProguardPrintConfiguration.h" // New ProGuard configuration
#include "Purity.h" // For defaults from config.
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "RedexResources.h"
#include "SanitizersConfig.h"
#include "Show.h"
#include "Timer.h"
#include "ToolsCommon.h"
#include "Walkers.h"
#include "Warning.h"

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
};

UNUSED void dump_args(const Arguments& args) {
  std::cout << "out_dir: " << args.out_dir << std::endl;
  std::cout << "verify_none_mode: " << args.redex_options.verify_none_enabled
            << std::endl;
  std::cout << "art_build: " << args.redex_options.is_art_build << std::endl;
  std::cout << "enable_pgi: " << args.redex_options.enable_pgi << std::endl;
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
      "ReBindRefsPass",        "BridgePass",
      "FinalInlinePassV2",     "DelSuperPass",
      "SingleImplPass",        "MethodInlinePass",
      "StaticReloPassV2",      "RemoveEmptyClassesPass",
      "ShortenSrcStringsPass", "RegAllocPass",
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

Arguments parse_args(int argc, char* argv[]) {
  Arguments args;
  args.out_dir = ".";
  args.config = default_config();

  namespace po = boost::program_options;
  po::options_description od(k_usage_header);
  od.add_options()("help,h", "print this help message");
  od.add_options()("reflect-config",
                   "print a reflection of the config and exit");
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
      "enable-pgi",
      po::bool_switch(&args.redex_options.enable_pgi)->default_value(false),
      "If not specified, Profile Guided Inlining will not be run.\n");
  od.add_options()(
      "disable-dex-hasher",
      po::bool_switch(&args.redex_options.disable_dex_hasher)
          ->default_value(false),
      "If specified, states that the current run disables dex hasher.\n");
  od.add_options()(
      "redacted",
      po::bool_switch(&args.redex_options.redacted)->default_value(false),
      "If specified then resulting dex files will have class data placed at"
      " the end of the file, i.e. last map item entry just before map list.\n");
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

    reflected_config["global"] = reflect_config(GlobalConfig::get().reflect());

    Json::Value pass_configs = Json::arrayValue;
    const auto& passes = PassRegistry::get().get_passes();
    for (size_t i = 0; i < passes.size(); ++i) {
      auto& pass = passes[i];
      pass_configs[static_cast<int>(i)] = reflect_config(pass->reflect());
    }
    reflected_config["passes"] = pass_configs;
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

  if (vm.count("dex-files")) {
    args.dex_files = vm["dex-files"].as<std::vector<std::string>>();
  } else {
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
  TRACE(MAIN, 2, "PGI enabled: %s",
        args.redex_options.enable_pgi ? "Yes" : "No");
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
    all["(initial)-registers"] =
        hashing::hash_to_string(initial_hash->registers_hash);
    all["(initial)-code"] = hashing::hash_to_string(initial_hash->code_hash);
    all["(initial)-signature"] =
        hashing::hash_to_string(initial_hash->signature_hash);
  }
  for (const auto& pass_info : mgr.get_pass_info()) {
    auto hash = pass_info.hash;
    if (hash) {
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
  Json::Value obj(Json::ValueType::objectValue);
  obj["num_2addr_instructions"] = Json::UInt(stats.to_2addr);
  obj["num_move_added_for_check_cast"] = Json::UInt(stats.move_for_check_cast);
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

Json::Value get_times() {
  Json::Value list(Json::arrayValue);
  for (const auto& t : Timer::get_times()) {
    Json::Value element;
    element[t.first] = std::round(t.second * 10) / 10.0;
    list.append(element);
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

Json::Value get_output_stats(
    const dex_stats_t& stats,
    const std::vector<dex_stats_t>& dexes_stats,
    const PassManager& mgr,
    const instruction_lowering::Stats& instruction_lowering_stats) {
  Json::Value d;
  d["total_stats"] = get_stats(stats);
  d["dexes_stats"] = get_detailed_stats(dexes_stats);
  d["pass_stats"] = get_pass_stats(mgr);
  d["pass_hashes"] = get_pass_hashes(mgr);
  d["lowering_stats"] = get_lowering_stats(instruction_lowering_stats);
  return d;
}

void write_debug_line_mapping(
    const std::string& debug_line_map_filename,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id,
    const std::unordered_map<DexCode*, std::vector<DebugLineItem>>&
        code_debug_lines,
    DexStoresVector& stores) {
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
  walk::methods(scope, [&](DexMethod* method) {
    auto dex_code = method->get_dex_code();
    if (dex_code == nullptr ||
        code_debug_lines.find(dex_code) == code_debug_lines.end()) {
      return;
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
  });
  ofs << line_out.str();
}

std::string get_dex_magic(std::vector<std::string>& dex_files) {
  always_assert_log(!dex_files.empty(), "APK contains no dex file\n");
  // Get dex magic from the first dex file since all dex magic
  // should be consistent within one APK.
  return load_dex_magic_from_dex(dex_files[0].c_str());
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
  for (const auto& pg_config_path : args.proguard_config_paths) {
    Timer time_pg_parsing("Parsed ProGuard config file");
    keep_rules::proguard_parser::parse_file(pg_config_path, &pg_config);
  }
  keep_rules::proguard_parser::remove_blocklisted_rules(&pg_config);

  const auto& pg_libs = pg_config.libraryjars;
  args.jar_paths.insert(pg_libs.begin(), pg_libs.end());

  std::set<std::string> library_jars;
  for (const auto& jar_path : args.jar_paths) {
    std::istringstream jar_stream(jar_path);
    std::string dependent_jar_path;
    while (std::getline(jar_stream, dependent_jar_path, ':')) {
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
  args.entry_data["jars"] = Json::arrayValue;
  if (!library_jars.empty()) {
    Timer t("Load library jars");

    for (const auto& library_jar : library_jars) {
      TRACE(MAIN, 1, "LIBRARY JAR: %s", library_jar.c_str());
      if (!load_jar_file(library_jar.c_str(), &external_classes)) {
        // Try again with the basedir
        std::string basedir_path = pg_config.basedirectory + "/" + library_jar;
        if (!load_jar_file(basedir_path.c_str())) {
          std::cerr << "error: library jar could not be loaded: " << library_jar
                    << std::endl;
          exit(EXIT_FAILURE);
        }
        args.entry_data["jars"].append(basedir_path);
      } else {
        auto abs_path = boost::filesystem::absolute(library_jar);
        args.entry_data["jars"].append(abs_path.string());
      }
    }
  }

  {
    Timer t("Deobfuscating dex elements");
    for (auto& store : stores) {
      apply_deobfuscated_names(store.get_dexen(), conf.get_proguard_map());
    }
  }
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("Processing proguard rules");

    bool keep_all_annotation_classes;
    json_config.get("keep_all_annotation_classes", true,
                    keep_all_annotation_classes);

    ConcurrentSet<const keep_rules::KeepSpec*> unused_rules =
        process_proguard_rules(conf.get_proguard_map(), scope, external_classes,
                               pg_config, keep_all_annotation_classes);
    if (unused_rules.size() > 0) {
      std::vector<std::string> out;
      for (const keep_rules::KeepSpec* keep_rule : unused_rules) {
        out.push_back(keep_rules::show_keep(*keep_rule));
      }
      // Make output deterministic
      std::sort(out.begin(), out.end());
      bool unused_rule_abort;
      conf.get_json_config().get("unused_keep_rule_abort", false,
                                 unused_rule_abort);
      if (unused_rule_abort) {
        exit(1);
        for (const auto& s : out) {
          fprintf(stderr, "%s not used\n", s.c_str());
        }
      }
      auto fd =
          fopen(conf.metafile("redex-unused-keep-rules.txt").c_str(), "w");
      for (const auto& s : out) {
        fprintf(fd, "%s\n", s.c_str());
      }
      fclose(fd);
    }
  }
  {
    Timer t("No Optimizations Rules");
    // this will change rstate of methods
    keep_rules::process_no_optimizations_rules(
        conf.get_no_optimizations_annos(), scope);
    monitor_count::mark_sketchy_methods_with_no_optimize(scope);
  }
  {
    Timer t("Initializing reachable classes");
    // init reachable will change rstate of classes, methods and fields
    init_reachable_classes(scope, ReachableClassesConfig(json_config));
  }
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

  instruction_lowering::Stats instruction_lowering_stats;
  {
    bool lower_with_cfg = true;
    conf.get_json_config().get("lower_with_cfg", true, lower_with_cfg);
    Timer t("Instruction lowering");
    instruction_lowering_stats =
        instruction_lowering::run(stores, lower_with_cfg);
  }

  TRACE(MAIN, 1, "Writing out new DexClasses...");
  const JsonWrapper& json_config = conf.get_json_config();

  LocatorIndex* locator_index = nullptr;
  if (json_config.get("emit_locator_strings", false)) {
    TRACE(LOC, 1,
          "Will emit class-locator strings for classloader optimization");
    locator_index = new LocatorIndex(make_locator_index(stores));
  }

  auto disable_method_similarity_order =
      json_config.get("disable_method_similarity_order", false);

  dex_stats_t output_totals;
  std::vector<dex_stats_t> output_dexes_stats;

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
  IODIMetadata iodi_metadata(redex_options.min_sdk);

  std::unique_ptr<PostLowering> post_lowering =
      redex_options.redacted ? PostLowering::create() : nullptr;

  if (post_lowering) {
    post_lowering->sync();
  }

  if (is_iodi(dik)) {
    Timer t("Compute initial IODI metadata");
    iodi_metadata.mark_methods(stores);
  }
  for (size_t store_number = 0; store_number < stores.size(); ++store_number) {
    auto& store = stores[store_number];
    Timer t("Writing optimized dexes");
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      auto this_dex_stats =
          write_classes_to_dex(redex_options,
                               redex::get_dex_output_name(output_dir, store, i),
                               &store.get_dexen()[i],
                               locator_index,
                               store_number,
                               i,
                               conf,
                               pos_mapper.get(),
                               needs_addresses ? &method_to_id : nullptr,
                               needs_addresses ? &code_debug_lines : nullptr,
                               is_iodi(dik) ? &iodi_metadata : nullptr,
                               stores[0].get_dex_magic(),
                               post_lowering.get(),
                               manager.get_redex_options().min_sdk,
                               disable_method_similarity_order);

      output_totals += this_dex_stats;
      output_dexes_stats.push_back(this_dex_stats);
    }
  }

  if (post_lowering) {
    post_lowering->run(stores);
    post_lowering->finalize(manager.apk_manager());
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
      write_debug_line_mapping(debug_line_map_filename, method_to_id,
                               code_debug_lines, stores);
    }
    if (is_iodi(dik)) {
      iodi_metadata.write(iodi_metadata_filename, method_to_id);
    }
    pos_mapper->write_map();
    stats["output_stats"] = get_output_stats(
        output_totals, output_dexes_stats, manager, instruction_lowering_stats);
    print_warning_summary();
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
    const auto& dexloc = cls->get_location();
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

} // namespace

int main(int argc, char* argv[]) {
  signal(SIGSEGV, crash_backtrace_handler);
  signal(SIGABRT, crash_backtrace_handler);
  signal(SIGINT, crash_backtrace_handler);
#if !IS_WINDOWS
  signal(SIGBUS, crash_backtrace_handler);
#endif

  // Only log one assert.
  block_multi_asserts(/*block=*/true);
  // For better stacks in abort dumps.
  set_abort_if_not_this_thread();

  std::string stats_output_path;
  Json::Value stats;
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

    RedexContext::set_record_keep_reasons(
        args.config.get("record_keep_reasons", false).asBool());

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
    const std::string& manifest_filename = apk_dir + "/AndroidManifest.xml";
    boost::optional<int32_t> maybe_sdk = get_min_sdk(manifest_filename);
    if (maybe_sdk != boost::none) {
      args.redex_options.min_sdk = *maybe_sdk;
    }

    redex_frontend(conf, args, *pg_config, stores, stats);
    GlobalConfig::get().parse_config(conf.get_json_config());

    // Initialize purity defaults, if set.
    purity::CacheConfig::parse_default(conf);

    auto const& passes = PassRegistry::get().get_passes();
    PassManager manager(passes, std::move(pg_config), args.config,
                        args.redex_options);

    if (manager.get_redex_options().is_art_build ||
        !args.config.get("enable_ab_experiments", false).asBool()) {
      ab_test::ABExperimentContext::force_preferred_mode();
    }

    {
      Timer t("Running optimization passes");
      manager.run_passes(stores, conf);
    }

    if (args.stop_pass_idx == boost::none) {
      // Call redex_backend by default
      redex_backend(conf, manager, stores, stats);
      if (args.config.get("emit_class_method_info_map", false).asBool()) {
        dump_class_method_info_map(conf.metafile(CLASS_METHOD_INFO_MAP),
                                   stores);
      }
    } else {
      redex::write_all_intermediate(conf, args.out_dir, args.redex_options,
                                    stores, args.entry_data);
    }

    stats_output_path = conf.metafile(
        args.config.get("stats_output", "redex-stats.txt").asString());
    {
      Timer t("Freeing global memory");
      delete g_redex;
    }
  }
  // now that all the timers are done running, we can collect the data
  stats["output_stats"]["time_stats"] = get_times();
  auto vm_stats = get_mem_stats();
  stats["output_stats"]["mem_stats"]["vm_peak"] =
      (Json::UInt64)vm_stats.vm_peak;
  stats["output_stats"]["mem_stats"]["vm_hwm"] = (Json::UInt64)vm_stats.vm_peak;
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

  return 0;
}
