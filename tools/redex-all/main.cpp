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
#include <streambuf>
#include <string>
#include <vector>

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/FileUtil.h>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "JarLoader.h"
#include "DexOutput.h"
#include "PassManager.h"
#include "ProguardLoader.h"
#include "ReachableClasses.h"
#include "RedexContext.h"
#include "Warning.h"

/**
 * Create a vector that registers all possible passes.  Forward-declared to make
 * it easy to separate open-source from non-public passes.
 */
std::vector<Pass*> create_passes();

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
    "  -w --warn    warning level:\n"
    "                   0: no warnings\n"
    "                   1: count of warnings\n"
    "                   2: full text of warnings\n"
    "  -Skey=string  Add a string value to the global config, overwriting the existing value if any\n"
    "                 Example: -Smy_param_name=foo\n"
    "  -SSomePassName.key=string\n"
    "               Add a string value to a pass config, overwriting the existing value if any\n"
    "                 Example: -SRenameClassesPass.class_rename=/foo/bar/data.txt\n"
    "  -Jkey=<json value>\n"
    "               Add a json value to the global config, overwriting the existing value if any.\n"
    "                 Example: -Smy_param_name={\"foo\": true}\n"
    "  -JSomePassName.key=<json value>\n"
    "               Add a json value to a pass config, overwriting the existing value if any\n"
    "                 Example: -SRenameClassesPass.class_rename=[1, 2, 3]\n"
    "\n"
    " Note: Be careful to properly escape JSON parameters, e.g. strings must be quoted.\n"
  );
}

////////////////////////////////////////////////////////////////////////////////

struct Arguments {
  Arguments() : config(nullptr) {}

  folly::dynamic config;
  std::string jar_path;
  std::string proguard_config;
  std::string seeds_filename;
  std::string out_dir;
};

bool parse_config(const char* config_file, Arguments& args) {
  std::ifstream config_stream(config_file);
  if (!config_stream) {
    fprintf(stderr, "ERROR: cannot find config file\n");
    return false;
  }
  std::stringstream config_content;
  config_content << config_stream.rdbuf();
  args.config = folly::parseJson(config_content.str());
  return true;
}

static folly::dynamic parse_json_value(std::string& value_string) {
  // If we just to parseJson() it will fail because that requires an object as the root.
  // Instead we wrap the value in a dummy object so that we can parse any JSON value:
  // array, object, string, boolean, number, or null
  std::string formatted = std::string("{\"dummy\":") + value_string + std::string("}");
  folly::dynamic temp = folly::parseJson(formatted.c_str());
  return temp["dummy"];
}

static bool add_value_to_config(folly::dynamic& config, std::string& key_value, bool is_json) {
  size_t equals_idx = key_value.find('=');
  size_t dot_idx = key_value.find('.');

  if (equals_idx != std::string::npos) {
    if (dot_idx != std::string::npos && dot_idx < equals_idx) {
      // Pass-specific config value specified with -Dpassname.key=value
      std::string pass = key_value.substr(0, dot_idx);
      std::string key = key_value.substr(dot_idx + 1, equals_idx - dot_idx - 1);
      std::string value_string = key_value.substr(equals_idx + 1);
      // Create empty object for the pass if it doesn't already exist
      if (config.find(pass.c_str()) == config.items().end()) {
        config[pass.c_str()] = folly::dynamic::object;
      }
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

folly::dynamic default_config() {
  using namespace folly;
  return dynamic::object
    ("redex",
     dynamic::object(
       "passes",
       dynamic::array(
         "ReBindRefsPass",
         "BridgePass",
         "SynthPass",
         "FinalInlinePass",
         "DelSuperPass",
         "SingleImplPass",
         "SimpleInlinePass",
         "StaticReloPass",
         "RemoveEmptyClassesPass",
         "ShortenSrcStringsPass"
       )));
}

int parse_args(int argc, char* argv[], Arguments& args) {
  const struct option options[] = {
    { "apkdir",  required_argument, 0, 'a' },
    { "config",  required_argument, 0, 'c' },
    { "jarpath", required_argument, 0, 'j'},
    { "proguard-config", required_argument, 0, 'p'},
    { "seeds", required_argument, 0, 's'},
    { "outdir",  required_argument, 0, 'o' },
    { "warn",    required_argument, 0, 'w' },
    { nullptr, 0, nullptr, 0 },
  };
  args.out_dir = ".";
  char c;

  std::vector<std::string> json_values_from_command_line;
  std::vector<std::string> string_values_from_command_line;

  args.config = default_config();
  const char* apk_dir = nullptr;

  while ((c = getopt_long(
            argc,
            argv,
            ":a:c:o:w:p:S::J::",
            &options[0],
            nullptr)) != -1) {
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
      args.jar_path = optarg;
      break;
    case 'p':
      args.proguard_config = optarg;
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
      return 0;  // getopt_long has printed an error
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

void output_stats(const char* path, const dex_output_stats_t& stats) {
  folly::dynamic d = folly::dynamic::object;
  d["num_types"] = stats.num_types;
  d["num_type_lists"] = stats.num_type_lists;
  d["num_classes"] = stats.num_classes;
  d["num_methods"] = stats.num_methods;
  d["num_method_refs"] = stats.num_method_refs;
  d["num_fields"] = stats.num_fields;
  d["num_field_refs"] = stats.num_field_refs;
  d["num_strings"] = stats.num_strings;
  d["num_protos"] = stats.num_protos;
  d["num_static_values"] = stats.num_static_values;
  d["num_annotations"] = stats.num_annotations;
  folly::writeFile(folly::toPrettyJson(d), path);
}

int main(int argc, char* argv[]) {
  signal(SIGSEGV, crash_backtrace);
  signal(SIGABRT, crash_backtrace);
  signal(SIGBUS, crash_backtrace);

  g_redex = new RedexContext();

  auto passes = create_passes();

  Arguments args;
  std::vector<KeepRule> rules;
  auto start = parse_args(argc, argv, args);
  if (!dir_is_writable(args.out_dir)) {
    fprintf(stderr, "outdir %s is not a writable directory\n",
            args.out_dir.c_str());
    exit(1);
  }

  if (!args.jar_path.empty()) {
    if (!load_jar_file(args.jar_path.c_str())) {
      fprintf(stderr, "ERROR: Unable to open jar %s\n",
              args.jar_path.c_str());
      start = 0;
    }
  } else {
    TRACE(MAIN, 1, "Skipping parsing a classpath jar\n");
  }

  if (!args.proguard_config.empty()) {
    if (!load_proguard_config_file(args.proguard_config.c_str(), &rules)) {
      fprintf(stderr, "ERROR: Unable to open proguard config %s\n",
              args.proguard_config.c_str());
      start = 0;
    }
  } else {
    TRACE(MAIN, 1, "Skipping parsing the proguard config file\n");
  }

  if (start == 0 || start == argc) {
    usage();
    exit(1);
  }

  DexClassesVector dexen;
  for (int i = start; i < argc; i++) {
    dexen.emplace_back(load_classes_from_dex(argv[i]));
  }

  if (!args.seeds_filename.empty()) {
    init_seed_classes(args.seeds_filename);
  }

  PassManager manager(passes, rules, args.config);
  manager.run_passes(dexen);

  TRACE(MAIN, 1, "Writing out new DexClasses...\n");

  LocatorIndex* locator_index = nullptr;
  if (args.config.getDefault("emit_locator_strings", false).asBool()) {
    TRACE(LOC, 1, "Will emit class-locator strings "
            "for classloader optimization\n");
    locator_index = new LocatorIndex(make_locator_index(dexen));
  }

  dex_output_stats_t totals;

  auto methodmapping = args.config.getDefault("method_mapping", "").asString();
  auto stats_output = args.config.getDefault("stats_output", "").asString();
  for (size_t i = 0; i < dexen.size(); i++) {
    std::stringstream ss;
    ss << args.out_dir + "/classes";
    if (i > 0) {
      ss << (i + 1);
    }
    ss << ".dex";
    auto stats = write_classes_to_dex(
      ss.str(),
      &dexen[i],
      locator_index,
      i,
      methodmapping.c_str());
    totals += stats;
  }
  output_stats(stats_output.c_str(), totals);
  print_warning_summary();
  delete g_redex;
  TRACE(MAIN, 1, "Done.\n");

  return 0;
}
