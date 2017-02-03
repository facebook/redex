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

#include <getopt.h>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json/json.h>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "JarLoader.h"
#include "PassManager.h"
#include "PassRegistry.h"
#include "ProguardConfiguration.h" // New ProGuard configuration
#include "ProguardParser.h" // New ProGuard Parser
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Timer.h"
#include "Warning.h"

static void usage() {
  fprintf(
      stderr,
      "usage: redex-all [opts...] dexes...\n\n"
      "  -a --apkdir  directory containing unzipped APK\n"
      "  -c --config  JSON-formatted config file\n"
      "  -o --outdir  output directory for optimized dexes\n"
      "  -j --jarpath Classpath jar\n"
      "  -p --proguard-config proguard config file\n"
      "  -s --seeds seeds file specifiying roots of classes to kept\n"
      "  -q --printseeds file to report seeds computed by redex\n"
      "  -w --warn    warning level:\n"
      "                   0: no warnings\n"
      "                   1: count of warnings\n"
      "                   2: full text of warnings\n"
      "  -Skey=string  Add a string value to the global config, overwriting "
      "the "
      "existing value if any\n"
      "                 Example: -Smy_param_name=foo\n"
      "  -SSomePassName.key=string\n"
      "               Add a string value to a pass config, overwriting the "
      "existing value if any\n"
      "                 Example: -SRenameClassesPass.class_rename="
      "/foo/bar/data.txt\n"
      "  -Jkey=<json value>\n"
      "               Add a json value to the global config, overwriting the "
      "existing value if any.\n"
      "                 Example: -Smy_param_name={\"foo\": true}\n"
      "  -JSomePassName.key=<json value>\n"
      "               Add a json value to a pass config, overwriting the "
      "existing value if any\n"
      "                 Example: -SRenameClassesPass.class_rename=[1, 2, 3]\n"
      "\n"
      " Note: Be careful to properly escape JSON parameters, e.g. strings "
      "must be quoted.\n");
}

/////////////////////////////////////////////////////////////////////////////

struct Arguments {
  Arguments() : config(Json::nullValue) {}

  Json::Value config;
  std::set<std::string> jar_paths;
  std::vector<std::string> proguard_config_paths;
  std::string seeds_filename;
  std::string out_dir;
  std::string printseeds;
};

bool parse_config(const char* config_file, Arguments& args) {
  std::ifstream config_stream(config_file);
  if (!config_stream) {
    fprintf(stderr, "ERROR: cannot find config file\n");
    return false;
  }
  config_stream >> args.config; // parse JSON
  return true;
}

static Json::Value parse_json_value(std::string& value_string) {
  std::stringstream temp_stream(value_string);
  Json::Value temp_json;
  temp_stream >> temp_json;
  return temp_json;
}

static bool add_value_to_config(Json::Value& config,
                                std::string& key_value,
                                bool is_json) {
  size_t equals_idx = key_value.find('=');
  size_t dot_idx = key_value.find('.');

  if (equals_idx != std::string::npos) {
    if (dot_idx != std::string::npos && dot_idx < equals_idx) {
      // Pass-specific config value specified with -Dpassname.key=value
      std::string pass = key_value.substr(0, dot_idx);
      std::string key = key_value.substr(dot_idx + 1, equals_idx - dot_idx - 1);
      std::string value_string = key_value.substr(equals_idx + 1);
      if (is_json) {
        config[pass.c_str()][key.c_str()] = parse_json_value(value_string);
      } else {
        config[pass.c_str()][key.c_str()] = value_string.c_str();
      }
    } else {
      // Global config value specified with -Dkey=value
      std::string key = key_value.substr(0, equals_idx);
      std::string value_string = key_value.substr(equals_idx + 1);
      if (is_json) {
        config[key.c_str()] = parse_json_value(value_string);
      } else {
        config[key.c_str()] = value_string.c_str();
      }
    }
    return true;
  }
  return false;
}

Json::Value default_config() {
  auto passes = {
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
  std::stringstream temp_json("{\"redex\":{\"passes\":[]}}");
  Json::Value cfg;
  temp_json >> cfg;
  for (auto const& pass : passes) {
    cfg["redex"]["passes"].append(pass);
  }
  return cfg;
}

int parse_args(int argc, char* argv[], Arguments& args) {
  const struct option options[] = {
      {"apkdir", required_argument, 0, 'a'},
      {"config", required_argument, 0, 'c'},
      {"jarpath", required_argument, 0, 'j'},
      {"proguard-config", required_argument, 0, 'p'},
      {"seeds", required_argument, 0, 's'},
      {"outdir", required_argument, 0, 'o'},
      {"warn", required_argument, 0, 'w'},
      {"printseeds", required_argument, 0, 'q'},
      {nullptr, 0, nullptr, 0},
  };
  args.out_dir = ".";
  int c;

  std::vector<std::string> json_values_from_command_line;
  std::vector<std::string> string_values_from_command_line;

  args.config = default_config();
  const char* apk_dir = nullptr;

  while ((c = getopt_long(
              argc, argv, ":a:c:j:p:q:s:o:w:S::J::", &options[0], nullptr)) !=
         -1) {
    switch (c) {
    case 'a':
      apk_dir = optarg;
      break;
    case 'c':
      if (!parse_config(optarg, args)) {
        return 0;
      }
      break;
    case 'o':
      args.out_dir = optarg;
      break;
    case 'j':
      args.jar_paths.emplace(optarg);
      TRACE(MAIN, 2, "Command line -j option: %s\n", optarg);
      break;
    case 'q':
      args.config["printseeds"] = optarg;
      break;
    case 'p':
      args.proguard_config_paths.push_back(optarg);
      break;
    case 'w':
      g_warning_level = OptWarningLevel(strtol(optarg, nullptr, 10));
      break;
    case 's':
      if (optarg) {
        args.seeds_filename = optarg;
      }
      break;
    case 'S':
      if (optarg) {
        std::string value(optarg);
        string_values_from_command_line.push_back(value);
      }
      break;
    case 'J':
      if (optarg) {
        std::string value(optarg);
        json_values_from_command_line.push_back(value);
      }
      break;
    case ':':
      fprintf(stderr, "ERROR: %s requires an argument\n", argv[optind - 1]);
      return 0;
    case '?':
      return 0; // getopt_long has printed an error
    default:
      abort();
    }
  }

  // We add these values to the config at the end so that they will always
  // overwrite values read from the config file regardless of the order of
  // arguments
  if (apk_dir) {
    args.config["apk_dir"] = apk_dir;
  }
  for (auto& key_value : json_values_from_command_line) {
    if (!add_value_to_config(args.config, key_value, true)) {
      fprintf(stderr, "Error parsing value -J%s\n", key_value.c_str());
    }
  }
  for (auto& key_value : string_values_from_command_line) {
    if (!add_value_to_config(args.config, key_value, false)) {
      fprintf(stderr, "Error parsing value -S%s\n", key_value.c_str());
    }
  }
  return optind;
}

bool dir_is_writable(const std::string& dir) {
  struct stat buf;
  if (stat(dir.c_str(), &buf) != 0) {
    return false;
  }
  if (!(buf.st_mode & S_IFDIR)) {
    return false;
  }
  return access(dir.c_str(), W_OK) == 0;
}

Json::Value get_stats(const dex_output_stats_t& stats) {
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
  for (auto pass_metrics : mgr.get_metrics()) {
    if (pass_metrics.second.empty()) {
      continue;
    }
    Json::Value pass;
    for (auto pass_metric : pass_metrics.second) {
      pass[pass_metric.first] = pass_metric.second;
    }
    all[pass_metrics.first] = pass;
  }
  return all;
}

Json::Value get_detailed_stats(
    const std::vector<dex_output_stats_t>& dexes_stats) {
  Json::Value dexes;
  int i = 0;
  for (const dex_output_stats_t& stats : dexes_stats) {
    dexes[i++] = get_stats(stats);
  }
  return dexes;
}

void output_stats(const char* path,
                  const dex_output_stats_t& stats,
                  const std::vector<dex_output_stats_t>& dexes_stats,
                  PassManager& mgr) {
  Json::Value d;
  d["total_stats"] = get_stats(stats);
  d["dexes_stats"] = get_detailed_stats(dexes_stats);
  d["pass_stats"] = get_pass_stats(mgr);
  Json::StyledStreamWriter writer;
  std::ofstream out(path);
  writer.write(out, d);
  out.close();
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

int main(int argc, char* argv[]) {
  signal(SIGSEGV, crash_backtrace_handler);
  signal(SIGABRT, crash_backtrace_handler);
  signal(SIGBUS, crash_backtrace_handler);

  Timer t("redex-all main()");

  g_redex = new RedexContext();

  Arguments args;
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
  std::set<std::string> library_jars;
  auto start = parse_args(argc, argv, args);
  if (!dir_is_writable(args.out_dir)) {
    fprintf(stderr,
            "outdir %s is not a writable directory\n",
            args.out_dir.c_str());
    exit(1);
  }

  redex::ProguardConfiguration pg_config;
  for (const auto pg_config_path : args.proguard_config_paths) {
    Timer time_pg_parsing("Parsed ProGuard config file " + pg_config_path);
    redex::proguard_parser::parse_file(pg_config_path, &pg_config);
  }

  auto const& pg_libs = pg_config.libraryjars;
  args.jar_paths.insert(pg_libs.begin(), pg_libs.end());

  for (const auto jar_path : args.jar_paths) {
    std::stringstream jar_stream(jar_path);
    std::string dependent_jar_path;
    while (std::getline(jar_stream, dependent_jar_path, ':')) {
      TRACE(MAIN,
            2,
            "Dependent JAR specified on command-line: %s\n",
            dependent_jar_path.c_str());
      library_jars.emplace(dependent_jar_path);
    }
  }

  if (start == 0 || start == argc) {
    usage();
    exit(1);
  }

  DexStore root_store("classes");
  DexStoresVector stores;
  stores.emplace_back(std::move(root_store));

  {
    Timer t("Load classes from dexes");
    for (int i = start; i < argc; i++) {
      const std::string filename(argv[i]);
      if (filename.compare(filename.size() - 3, 3, "dex") == 0) {
        DexClasses classes = load_classes_from_dex(filename.c_str());
        stores[0].add_classes(std::move(classes));
      } else {
        DexMetadata store_metadata;
        store_metadata.parse(filename);
        DexStore store(store_metadata);
        for (auto file_path : store_metadata.get_files()) {
          DexClasses classes = load_classes_from_dex(file_path.c_str());
          store.add_classes(std::move(classes));
        }
        stores.emplace_back(std::move(store));
      }
    }
  }

  if (!library_jars.empty()) {
    Timer t("Load library jars");
    for (const auto& library_jar : library_jars) {
      TRACE(MAIN, 1, "LIBRARY JAR: %s\n", library_jar.c_str());
      if (!load_jar_file(library_jar.c_str())) {
        // Try again with the basedir
        std::string basedir_path =
            pg_config.basedirectory + "/" + library_jar.c_str();
        if (!load_jar_file(basedir_path.c_str())) {
          // Look for buck-out in path.
          auto buck_out_pos = library_jar.find("buck-out");
          if (buck_out_pos != std::string::npos) {
            std::string buck_out_path = library_jar.substr(buck_out_pos);
            if (!load_jar_file(buck_out_path.c_str())) {
              fprintf(stderr,
                      "ERROR: Library jar could not be loaded: %s\n",
                      library_jar.c_str());
              exit(1);
            }
          }
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
  cfg.using_seeds = false;
  cfg.outdir = args.out_dir;
  if (!args.seeds_filename.empty()) {
    Timer t("Initialized seed classes from incoming seeds file " +
            args.seeds_filename);
    auto nseeds =
        init_seed_classes(args.seeds_filename, cfg.get_proguard_map());
    cfg.using_seeds = nseeds > 0;
  }

  auto const& passes = PassRegistry::get().get_passes();
  PassManager manager(passes, pg_config, args.config);
  {
    Timer t("Running optimization passes");
    manager.run_passes(stores, cfg);
  }

  TRACE(MAIN, 1, "Writing out new DexClasses...\n");

  LocatorIndex* locator_index = nullptr;
  if (args.config.get("emit_locator_strings", false).asBool()) {
    TRACE(LOC,
          1,
          "Will emit class-locator strings for classloader optimization\n");
    locator_index = new LocatorIndex(make_locator_index(stores));
  }

  dex_output_stats_t totals;
  std::vector<dex_output_stats_t> dexes_stats;

  auto pos_output =
      cfg.metafile(args.config.get("line_number_map", "").asString());
  auto pos_output_v2 =
      cfg.metafile(args.config.get("line_number_map_v2", "").asString());
  std::unique_ptr<PositionMapper> pos_mapper(
      PositionMapper::make(pos_output, pos_output_v2));
  for (auto& store : stores) {
    Timer t("Writing optimized dexes");
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      std::stringstream ss;
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
      auto stats = write_classes_to_dex(ss.str(),
                                        &store.get_dexen()[i],
                                        locator_index,
                                        i,
                                        cfg,
                                        args.config,
                                        pos_mapper.get());
      totals += stats;
      dexes_stats.push_back(stats);
    }
  }

  {
    Timer t("Writing stats");
    auto stats_output =
        cfg.metafile(args.config.get("stats_output", "").asString());
    auto method_move_map =
        cfg.metafile(args.config.get("method_move_map", "").asString());
    pos_mapper->write_map();
    output_stats(stats_output.c_str(), totals, dexes_stats, manager);
    output_moved_methods_map(method_move_map.c_str(), cfg);
    print_warning_summary();
  }
  {
    Timer t("Freeing global memory");
    delete g_redex;
  }
  TRACE(MAIN, 1, "Done.\n");

  return 0;
}
