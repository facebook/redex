/**
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
#include <set>
#include <streambuf>
#include <string>
#include <vector>

#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <json/json.h>

#include "CommentFilter.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "IODIMetadata.h"
#include "InstructionLowering.h"
#include "JarLoader.h"
#include "NoOptimizationsMatcher.h"
#include "OptData.h"
#include "PassRegistry.h"
#include "ProguardConfiguration.h" // New ProGuard configuration
#include "ProguardMatcher.h"
#include "ProguardParser.h" // New ProGuard Parser
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "RedexResources.h"
#include "Timer.h"
#include "ToolsCommon.h"
#include "Walkers.h"
#include "Warning.h"

namespace {
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
  std::string output_ir_dir;
  RedexOptions redex_options;
};

UNUSED void dump_args(const Arguments& args) {
  std::cout << "out_dir: " << args.out_dir << std::endl;
  std::cout << "verify_none_mode: " << args.redex_options.verify_none_enabled
            << std::endl;
  std::cout << "art_build: " << args.redex_options.is_art_build << std::endl;
  std::cout << "enable_instrument_pass: "
            << args.redex_options.instrument_pass_enabled << std::endl;
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
      "ReBindRefsPass",        "BridgePass",     "SynthPass",
      "FinalInlinePass",       "DelSuperPass",   "SingleImplPass",
      "MethodInlinePass",      "StaticReloPass", "RemoveEmptyClassesPass",
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

Arguments parse_args(int argc, char* argv[]) {
  Arguments args;
  args.out_dir = ".";
  args.config = default_config();

  namespace po = boost::program_options;
  po::options_description od(k_usage_header);
  od.add_options()("help,h", "print this help message");
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
      TRACE(MAIN, 2, "Command line -j option: %s\n", e.c_str());
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

  // Development usage only
  if (vm.count("stop-pass")) {
    args.stop_pass_idx = vm["stop-pass"].as<int>();
  }

  if (vm.count("output-ir")) {
    args.output_ir_dir = vm["output-ir"].as<std::string>();
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
    if (idx > 0 && passes_list[idx - 1].asString() != "RegAllocPass") {
      passes_list.append("RegAllocPass");
    }
    if (args.output_ir_dir.empty() ||
        !redex::dir_is_writable(args.output_ir_dir)) {
      std::cerr << "output-ir is empty or not writable" << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  TRACE(MAIN, 2, "Verify-none mode: %s\n",
        args.redex_options.verify_none_enabled ? "Yes" : "No");
  TRACE(MAIN, 2, "Art build: %s\n",
        args.redex_options.is_art_build ? "Yes" : "No");
  TRACE(MAIN, 2, "Enable InstrumentPass: %s\n",
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
      pass[pass_metric.first] = pass_metric.second;
    }
    all[pass_info.name] = pass;
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
  using ms = std::chrono::milliseconds;
  using std::chrono::duration_cast;
  Json::Value list(Json::arrayValue);
  for (const auto& event : Timer::get_times()) {
    Json::Value element;
    element["event"] = event.name;
    element["depth"] = event.depth;
    element["start"] = static_cast<Json::Int64>(
        duration_cast<ms>(event.start.time_since_epoch()).count());
    element["end"] = static_cast<Json::Int64>(
        duration_cast<ms>(event.end.time_since_epoch()).count());
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
  d["lowering_stats"] = get_lowering_stats(instruction_lowering_stats);
  return d;
}

void output_moved_methods_map(const char* path, const ConfigFiles& cfg) {
  // print out moved methods map
  if (cfg.save_move_map() && strcmp(path, "")) {
    FILE* fd = fopen(path, "w");
    if (fd == nullptr) {
      perror("Error opening method move file");
      return;
    }
    auto const& move_map = cfg.get_moved_methods_map();
    std::string dummy = "dummy";
    for (const auto& it : move_map) {
      MethodTuple mt = it.first;
      auto cls_name = std::get<0>(mt);
      auto meth_name = std::get<1>(mt);
      auto src_file = std::get<2>(mt);
      auto ren_to_cls_name = it.second->get_type()->get_name()->c_str();
      const char* src_string;
      if (src_file != nullptr) {
        src_string = src_file->c_str();
      } else {
        src_string = dummy.c_str();
      }
      fprintf(fd,
              "%s %s (%s) -> %s \n",
              cls_name->c_str(),
              meth_name->c_str(),
              src_string,
              ren_to_cls_name);
    }
    fclose(fd);
  } else {
    TRACE(MAIN, 1, "No method move map data structure!\n");
  }
}

void write_debug_line_mapping(
    const std::string& debug_line_mapping_filename_v2,
    const std::unordered_map<DexMethod*, uint64_t>& method_to_id,
    const std::unordered_map<DexCode*, std::vector<DebugLineItem>>&
        code_debug_lines,
    DexStoresVector& stores) {
  if (debug_line_mapping_filename_v2.empty()) {
    return;
  }
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
  std::ofstream ofs(debug_line_mapping_filename_v2.c_str(),
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

const std::string get_dex_magic(std::vector<std::string>& dex_files) {
  always_assert_log(dex_files.size() > 0, "APK contains no dex file\n");
  // Get dex magic from the first dex file since all dex magic
  // should be consistent within one APK.
  return load_dex_magic_from_dex(dex_files[0].c_str());
}

static void assert_dex_magic_consistency(const std::string& source,
                                         const std::string& target) {
  always_assert_log(source.compare(target) == 0,
                    "APK contains dex file of different versions: %s vs %s\n",
                    source.c_str(), target.c_str());
}

/**
 * Pre processing steps: load dex and configurations
 */
void redex_frontend(ConfigFiles& cfg, /* input */
                    Arguments& args, /* inout */
                    redex::ProguardConfiguration& pg_config,
                    DexStoresVector& stores,
                    Json::Value& stats) {
  Timer redex_frontend_timer("Redex_frontend");
  for (const auto& pg_config_path : args.proguard_config_paths) {
    Timer time_pg_parsing("Parsed ProGuard config file");
    redex::proguard_parser::parse_file(pg_config_path, &pg_config);
  }

  const auto& pg_libs = pg_config.libraryjars;
  args.jar_paths.insert(pg_libs.begin(), pg_libs.end());

  std::set<std::string> library_jars;
  for (const auto& jar_path : args.jar_paths) {
    std::istringstream jar_stream(jar_path);
    std::string dependent_jar_path;
    while (std::getline(jar_stream, dependent_jar_path, ':')) {
      TRACE(MAIN,
            2,
            "Dependent JAR specified on command-line: %s\n",
            dependent_jar_path.c_str());
      library_jars.emplace(dependent_jar_path);
    }
  }

  DexStore root_store("classes");
  // Only set dex magic to root DexStore since all dex magic
  // should be consistent within one APK.
  root_store.set_dex_magic(get_dex_magic(args.dex_files));
  stores.emplace_back(std::move(root_store));

  {
    Timer t("Load classes from dexes");
    dex_stats_t input_totals;
    std::vector<dex_stats_t> input_dexes_stats;
    for (const auto& filename : args.dex_files) {
      if (filename.size() >= 5 &&
          filename.compare(filename.size() - 4, 4, ".dex") == 0) {
        assert_dex_magic_consistency(stores[0].get_dex_magic(),
                                     load_dex_magic_from_dex(filename.c_str()));
        dex_stats_t dex_stats;
        DexClasses classes =
            load_classes_from_dex(filename.c_str(), &dex_stats);
        input_totals += dex_stats;
        input_dexes_stats.push_back(dex_stats);
        stores[0].add_classes(std::move(classes));
      } else {
        DexMetadata store_metadata;
        store_metadata.parse(filename);
        DexStore store(store_metadata);
        for (const auto& file_path : store_metadata.get_files()) {
          assert_dex_magic_consistency(
              stores[0].get_dex_magic(),
              load_dex_magic_from_dex(file_path.c_str()));
          dex_stats_t dex_stats;
          DexClasses classes =
              load_classes_from_dex(file_path.c_str(), &dex_stats);

          input_totals += dex_stats;
          input_dexes_stats.push_back(dex_stats);
          store.add_classes(std::move(classes));
        }
        stores.emplace_back(std::move(store));
      }
    }
    stats["input_stats"] = get_input_stats(input_totals, input_dexes_stats);
  }

  Scope external_classes;
  args.entry_data["jars"] = Json::arrayValue;
  if (!library_jars.empty()) {
    Timer t("Load library jars");
    const JsonWrapper& json_cfg = cfg.get_json_config();
    read_dup_class_whitelist(json_cfg);

    for (const auto& library_jar : library_jars) {
      TRACE(MAIN, 1, "LIBRARY JAR: %s\n", library_jar.c_str());
      if (!load_jar_file(library_jar.c_str(), &external_classes)) {
        // Try again with the basedir
        std::string basedir_path =
            pg_config.basedirectory + "/" + library_jar.c_str();
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
      apply_deobfuscated_names(store.get_dexen(), cfg.get_proguard_map());
    }
  }
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("Processing proguard rules");
    process_proguard_rules(cfg.get_proguard_map(), scope, external_classes,
                           pg_config);
  }
  {
    Timer t("No Optimizations Rules");
    // this will change rstate of methods
    redex::process_no_optimizations_rules(cfg.get_no_optimizations_annos(),
                                          scope);
  }
  {
    Timer t("Initializing reachable classes");
    // init reachable will change rstate of classes, methods and fields
    init_reachable_classes(scope, cfg.get_json_config(),
                           cfg.get_no_optimizations_annos());
  }
}

/**
 * Post processing steps: write dex and collect stats
 */
void redex_backend(const PassManager& manager,
                   const std::string& output_dir,
                   const ConfigFiles& cfg,
                   DexStoresVector& stores,
                   Json::Value& stats) {
  Timer redex_backend_timer("Redex_backend");
  instruction_lowering::Stats instruction_lowering_stats;
  {
    bool lower_with_cfg = false;
    cfg.get_json_config().get("lower_with_cfg", false, lower_with_cfg);
    Timer t("Instruction lowering");
    instruction_lowering_stats =
        instruction_lowering::run(stores, lower_with_cfg);
  }

  TRACE(MAIN, 1, "Writing out new DexClasses...\n");
  const JsonWrapper& json_cfg = cfg.get_json_config();

  LocatorIndex* locator_index = nullptr;
  bool emit_name_based_locators = false;
  if (json_cfg.get("emit_locator_strings", false)) {
    emit_name_based_locators =
        json_cfg.get("emit_name_based_locator_strings", false);
    TRACE(LOC, 1,
          "Will emit%s class-locator strings for classloader optimization\n",
          emit_name_based_locators ? " name-based" : "");
    locator_index =
        new LocatorIndex(make_locator_index(stores, emit_name_based_locators));
  }

  dex_stats_t output_totals;
  std::vector<dex_stats_t> output_dexes_stats;

  auto pos_output =
      cfg.metafile(json_cfg.get("line_number_map", std::string()));
  auto pos_output_v2 =
      cfg.metafile(json_cfg.get("line_number_map_v2", std::string()));
  auto debug_line_mapping_filename_v2 =
      cfg.metafile(json_cfg.get("debug_line_method_map_v2", std::string()));
  auto iodi_metadata_filename =
      cfg.metafile(json_cfg.get("iodi_metadata", std::string()));
  bool iodi_enable_overloaded_methods =
      json_cfg.get("iodi_enable_overloaded_methods", false);
  if (manager.get_redex_options().is_art_build &&
      !iodi_metadata_filename.empty()) {
    iodi_metadata_filename = "";
    fprintf(stderr, "Disabling IODI because this is an ART build.\n");
  }
  if ((debug_line_mapping_filename_v2.empty() || pos_output_v2.empty()) &&
      !iodi_metadata_filename.empty()) {
    fprintf(stderr,
            "[WARNING] IODI will not be used because it requires"
            " debug_line_method_map_v2 and line_number_map_v2 to be set"
            " (these artifacts are required for leaveraging iodi_metadata)!\n");
    iodi_metadata_filename = "";
  } else if (!iodi_metadata_filename.empty()) {
    TRACE(IODI, 1, "Attempting to use IODI, enabling overloaded methods: %s\n",
          iodi_enable_overloaded_methods ? "yes" : "no");
  }

  std::unique_ptr<PositionMapper> pos_mapper(
      PositionMapper::make(pos_output, pos_output_v2));
  std::unordered_map<DexMethod*, uint64_t> method_to_id;
  std::unordered_map<DexCode*, std::vector<DebugLineItem>> code_debug_lines;
  IODIMetadata iodi_metadata(iodi_enable_overloaded_methods);
  bool needs_method_to_id = !iodi_metadata_filename.empty() ||
                            !debug_line_mapping_filename_v2.empty();
  if (!iodi_metadata_filename.empty()) {
    Timer t("Compute initial IODI metadata");
    iodi_metadata.mark_methods(stores);
  }
  for (size_t store_number = 0; store_number < stores.size(); ++store_number) {
    auto& store = stores[store_number];
    Timer t("Writing optimized dexes");
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      std::ostringstream ss;
      ss << output_dir << "/" << store.get_name();
      if (store.get_name().compare("classes") == 0) {
        // primary/secondary dex store, primary has no numeral and secondaries
        // start at 2
        if (i > 0) {
          ss << (i + 1);
        }
      } else {
        // other dex stores do not have a primary,
        // so it makes sense to start at 2
        ss << (i + 2);
      }
      ss << ".dex";
      auto this_dex_stats = write_classes_to_dex(
          ss.str(),
          &store.get_dexen()[i],
          locator_index,
          emit_name_based_locators,
          store_number,
          i,
          cfg,
          pos_mapper.get(),
          needs_method_to_id ? &method_to_id : nullptr,
          debug_line_mapping_filename_v2.empty() ? nullptr : &code_debug_lines,
          iodi_metadata_filename.empty() ? nullptr : &iodi_metadata,
          stores[0].get_dex_magic());
      output_totals += this_dex_stats;
      output_dexes_stats.push_back(this_dex_stats);
    }
  }

  if (!iodi_metadata_filename.empty()) {
    Timer t("Compute IODI caller metadata");
    iodi_metadata.mark_callers();
  }

  {
    Timer t("Writing opt decisions data");
    const Json::Value& opt_decisions_args =
        cfg.get_json_config()["opt_decisions"];
    if (opt_decisions_args.get("enable_logs", false).asBool()) {
      auto opt_decisions_output_path = cfg.metafile(
          opt_decisions_args.get("output_file_name", "").asString());
      auto opt_data =
          opt_metadata::OptDataMapper::get_instance().serialize_sql();
      Json::StyledStreamWriter writer;
      {
        std::ofstream opt_data_out(opt_decisions_output_path);
        writer.write(opt_data_out, opt_data);
      }
    }
  }

  {
    Timer t("Writing stats");
    auto method_move_map =
        cfg.metafile(json_cfg.get("method_move_map", std::string()));
    write_debug_line_mapping(debug_line_mapping_filename_v2, method_to_id,
                             code_debug_lines, stores);
    iodi_metadata.write(iodi_metadata_filename, method_to_id);
    pos_mapper->write_map();
    stats["output_stats"] = get_output_stats(
        output_totals, output_dexes_stats, manager, instruction_lowering_stats);
    output_moved_methods_map(method_move_map.c_str(), cfg);
    print_warning_summary();
  }
}

void dump_class_method_info_map(const std::string file_path,
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

    assert(!class_map.count(cls));
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
#ifndef _MSC_VER
  signal(SIGBUS, crash_backtrace_handler);
#endif

  std::string timings_output_path;
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

    auto pg_config = std::make_unique<redex::ProguardConfiguration>();
    DexStoresVector stores;
    ConfigFiles cfg(args.config, args.out_dir);

    std::string apk_dir;
    cfg.get_json_config().get("apk_dir", "", apk_dir);
    const std::string& manifest_filename = apk_dir + "/AndroidManifest.xml";
    boost::optional<int32_t> maybe_sdk = get_min_sdk(manifest_filename);
    if (maybe_sdk != boost::none) {
      args.redex_options.min_sdk = *maybe_sdk;
    }

    redex_frontend(cfg, args, *pg_config, stores, stats);

    auto const& passes = PassRegistry::get().get_passes();
    PassManager manager(passes, std::move(pg_config), args.config,
                        args.redex_options);
    {
      Timer t("Running optimization passes");
      manager.run_passes(stores, cfg);
    }

    if (args.stop_pass_idx == boost::none) {
      // Call redex_backend by default
      redex_backend(manager, args.out_dir, cfg, stores, stats);
      if (!args.config.get("class_method_info_map", "").empty()) {
        dump_class_method_info_map(
            cfg.metafile(
                args.config.get("class_method_info_map", "").asString()),
            stores);
      }
    } else {
      redex::write_all_intermediate(cfg, args.output_ir_dir, args.redex_options,
                                    stores, args.entry_data);
    }

    std::string stats_output_path =
        cfg.metafile(args.config.get("stats_output", "").asString());
    {
      std::ofstream out(stats_output_path);
      Json::StyledStreamWriter writer;
      writer.write(out, stats);
    }

    timings_output_path =
        cfg.metafile(args.config.get("timings_output", "").asString());
    {
      Timer t("Freeing global memory");
      delete g_redex;
    }
  }

  // now that all the timers are done running, we can collect the data
  {
    std::ofstream out(timings_output_path);
    Json::StyledStreamWriter writer;
    writer.write(out, get_times());
  }

  TRACE(MAIN, 1, "Done.\n");
  return 0;
}
