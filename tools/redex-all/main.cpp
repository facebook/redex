/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

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
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <boost/iostreams/filtering_stream.hpp> // uses deprecated auto_ptr
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <json/json.h>

#include "CommentFilter.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "InstructionLowering.h"
#include "JarLoader.h"
#include "PassManager.h"
#include "PassRegistry.h"
#include "ProguardConfiguration.h" // New ProGuard configuration
#include "ProguardParser.h" // New ProGuard Parser
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Timer.h"
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
  bool verify_none_mode{false};
  bool art_build{false};
};

UNUSED void dump_args(const Arguments& args) {
  std::cout << "out_dir: " << args.out_dir << std::endl;
  std::cout << "verify_none_mode: " << args.verify_none_mode << std::endl;
  std::cout << "art_build: " << args.art_build << std::endl;
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

Json::Value parse_config(const std::string& config_file) {
  std::ifstream config_stream(config_file);
  if (!config_stream) {
    std::cerr << "error: cannot find config file: " << config_file << std::endl;
    exit(EXIT_FAILURE);
  }

  boost::iostreams::filtering_istream inbuf;
  inbuf.push(CommentFilter());
  inbuf.push(config_stream);
  Json::Value ret;
  inbuf >> ret; // parse JSON
  return ret;
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
      "ReBindRefsPass",
      "BridgePass",
      "SynthPass",
      "FinalInlinePass",
      "DelSuperPass",
      "SingleImplPass",
      "SimpleInlinePass",
      "StaticReloPass",
      "RemoveEmptyClassesPass",
      "ShortenSrcStringsPass",
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
  od.add_options()("warn,w",
                   po::value<std::vector<int>>(),
                   "warning level:\n"
                   "  0: no warnings\n"
                   "  1: count of warnings\n"
                   "  2: full text of warnings");
  od.add_options()(
      "verify-none-mode",
      po::bool_switch(&args.verify_none_mode)->default_value(false),
      "run redex in verify-none mode\n"
      "  \tThis will activate optimization passes or code in some passes that "
      "wouldn't normally operate with verification enabled.");
  od.add_options()(
      "is-art-build",
      "If specified, states that the current build is art specific.\n");
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
  od.add_options()(
      "dex-files", po::value<std::vector<std::string>>(), "dex files");

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
    args.config = parse_config(take_last(vm["config"]));
  }

  if (vm.count("outdir")) {
    args.out_dir = take_last(vm["outdir"]);
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
    args.config["apk_dir"] = take_last(vm["apkdir"]);
  }

  if (vm.count("printseeds")) {
    args.config["printseeds"] = take_last(vm["printseeds"]);
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

  if (vm.count("is-art-build")) {
    args.art_build = true;
  } else {
    args.art_build = false;
  }

  TRACE(
      MAIN, 2, "Verify-none mode: %s\n", args.verify_none_mode ? "Yes" : "No");
  return args;
}

bool dir_is_writable(const std::string& dir) {
  if (!boost::filesystem::is_directory(dir)) {
    return false;
  }
#ifdef _MSC_VER
  return _access(dir.c_str(), 2) == 0;
#else
  return access(dir.c_str(), W_OK) == 0;
#endif
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
  Json::Value list(Json::arrayValue);
  for (auto t : Timer::get_times()) {
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
  d["lowering_stats"] = get_lowering_stats(instruction_lowering_stats);
  return d;
}

void output_moved_methods_map(const char* path, ConfigFiles& cfg) {
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
} // namespace

int main(int argc, char* argv[]) {
  signal(SIGSEGV, crash_backtrace_handler);
  signal(SIGABRT, crash_backtrace_handler);
#ifndef _MSC_VER
  signal(SIGBUS, crash_backtrace_handler);
#endif

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

    if (!dir_is_writable(args.out_dir)) {
      std::cerr << "error: outdir is not a writable directory: " << args.out_dir
                << std::endl;
      exit(EXIT_FAILURE);
    }

    RedexContext::set_next_release_gate(
        args.config.get("next_release_gate", false).asBool());

    redex::ProguardConfiguration pg_config;
    for (const auto pg_config_path : args.proguard_config_paths) {
      Timer time_pg_parsing("Parsed ProGuard config file");
      redex::proguard_parser::parse_file(pg_config_path, &pg_config);
    }

    const auto& pg_libs = pg_config.libraryjars;
    args.jar_paths.insert(pg_libs.begin(), pg_libs.end());

    std::set<std::string> library_jars;
    for (const auto jar_path : args.jar_paths) {
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
    DexStoresVector stores;
    stores.emplace_back(std::move(root_store));

    dex_stats_t input_totals;
    std::vector<dex_stats_t> input_dexes_stats;

    {
      Timer t("Load classes from dexes");
      for (const auto& filename : args.dex_files) {
        if (filename.size() >= 5 &&
            filename.compare(filename.size() - 4, 4, ".dex") == 0) {
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
          for (auto file_path : store_metadata.get_files()) {
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
    }

    Scope external_classes;
    if (!library_jars.empty()) {
      Timer t("Load library jars");
      for (const auto& library_jar : library_jars) {
        TRACE(MAIN, 1, "LIBRARY JAR: %s\n", library_jar.c_str());
        if (!load_jar_file(library_jar.c_str(), &external_classes)) {
          // Try again with the basedir
          std::string basedir_path =
              pg_config.basedirectory + "/" + library_jar.c_str();
          if (!load_jar_file(basedir_path.c_str())) {
            std::cerr << "error: library jar could not be loaded: "
                      << library_jar << std::endl;
            exit(EXIT_FAILURE);
          }
        }
      }
    }

    ConfigFiles cfg(args.config);
    {
      Timer t("Deobfuscating dex elements");
      for (auto& store : stores) {
        apply_deobfuscated_names(store.get_dexen(), cfg.get_proguard_map());
      }
    }
    cfg.outdir = args.out_dir;

    auto const& passes = PassRegistry::get().get_passes();
    PassManager manager(passes, pg_config, args.config, args.verify_none_mode,
                        args.art_build);
    instruction_lowering::Stats instruction_lowering_stats;
    {
      Timer t("Running optimization passes");
      manager.run_passes(stores, external_classes, cfg);
      instruction_lowering_stats = instruction_lowering::run(stores);
    }

    TRACE(MAIN, 1, "Writing out new DexClasses...\n");

    LocatorIndex* locator_index = nullptr;
    if (args.config.get("emit_locator_strings", false).asBool()) {
      TRACE(LOC,
            1,
            "Will emit class-locator strings for classloader optimization\n");
      locator_index = new LocatorIndex(make_locator_index(stores));
    }

    dex_stats_t output_totals;
    std::vector<dex_stats_t> output_dexes_stats;

    auto pos_output =
        cfg.metafile(args.config.get("line_number_map", "").asString());
    auto pos_output_v2 =
        cfg.metafile(args.config.get("line_number_map_v2", "").asString());
    std::unique_ptr<PositionMapper> pos_mapper(
        PositionMapper::make(pos_output, pos_output_v2));
    for (auto& store : stores) {
      Timer t("Writing optimized dexes");
      for (size_t i = 0; i < store.get_dexen().size(); i++) {
        std::ostringstream ss;
        ss << args.out_dir << "/" << store.get_name();
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
        auto this_dex_stats = write_classes_to_dex(ss.str(),
                                                   &store.get_dexen()[i],
                                                   locator_index,
                                                   i,
                                                   cfg,
                                                   args.config,
                                                   pos_mapper.get());
        output_totals += this_dex_stats;
        output_dexes_stats.push_back(this_dex_stats);
      }
    }

    {
      Timer t("Writing stats");
      stats_output_path =
          cfg.metafile(args.config.get("stats_output", "").asString());
      auto method_move_map =
          cfg.metafile(args.config.get("method_move_map", "").asString());
      pos_mapper->write_map();
      stats["input_stats"] = get_input_stats(input_totals, input_dexes_stats);
      stats["output_stats"] = get_output_stats(output_totals,
                                               output_dexes_stats,
                                               manager,
                                               instruction_lowering_stats);
      output_moved_methods_map(method_move_map.c_str(), cfg);
      print_warning_summary();
    }
    {
      Timer t("Freeing global memory");
      delete g_redex;
    }
    TRACE(MAIN, 1, "Done.\n");
  }

  // now that all the timers are done running, we can collect the data
  stats["output_stats"]["time_stats"] = get_times();
  Json::StyledStreamWriter writer;
  {
    std::ofstream out(stats_output_path);
    writer.write(out, stats);
  }

  return 0;
}
