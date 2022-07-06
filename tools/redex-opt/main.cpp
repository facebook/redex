/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include <json/json.h>

#include "DexClass.h"
#include "DexLoader.h"
#include "PassRegistry.h"
#include "RedexContext.h"
#include "Timer.h"
#include "ToolsCommon.h"

namespace {

struct Arguments {
  std::string input_ir_dir;
  std::string output_ir_dir;
  std::vector<std::string> pass_names;
  RedexOptions redex_options;
  std::string config_file;
  std::vector<std::string> s_args;
  std::vector<std::string> j_args;
};

Arguments parse_args(int argc, char* argv[]) {
  namespace po = boost::program_options;
  po::options_description desc(
      "Run one pass with dex and IR meta as input and output");
  desc.add_options()("help,h", "produce help message");
  desc.add_options()("input-ir,i", po::value<std::string>(),
                     "input dex and IR meta directory");
  desc.add_options()("output-ir,o",
                     po::value<std::string>(),
                     "output dex and IR meta directory");
  desc.add_options()("pass-name,p", po::value<std::vector<std::string>>(),
                     "pass name");
  desc.add_options()("config,c",
                     po::value<std::string>(),
                     "A JSON-formatted config file to replace the one from "
                     "{input-ir}/entry.json");
  desc.add_options()(",S",
                     po::value<std::vector<std::string>>(), // Accumulation
                     "-Skey=string\n"
                     "  \tAdd a string value to the global config, overwriting "
                     "the existing value if any\n"
                     "    \te.g. -Smy_param_name=foo\n"
                     "-Spass_name.key=string\n"
                     "  \tAdd a string value to a pass"
                     "config, overwriting the existing value if any\n"
                     "    \te.g. -SMyPass.config=\"foo bar\"");
  desc.add_options()(
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

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help")) {
    desc.print(std::cout);
    exit(EXIT_SUCCESS);
  }

  Arguments args;

  if (vm.count("input-ir")) {
    args.input_ir_dir = vm["input-ir"].as<std::string>();
  }

  if (vm.count("output-ir")) {
    args.output_ir_dir = vm["output-ir"].as<std::string>();
  }
  if (args.output_ir_dir.empty()) {
    std::cerr << "output-dir is empty\n";
    exit(EXIT_FAILURE);
  }
  std::string meta_dir = args.output_ir_dir + "/meta";
  boost::filesystem::create_directories(meta_dir);
  if (!boost::filesystem::is_directory(meta_dir)) {
    std::cerr << "Could not create " << meta_dir << std::endl;
    exit(EXIT_FAILURE);
  }

  if (vm.count("pass-name")) {
    args.pass_names = vm["pass-name"].as<std::vector<std::string>>();
  }

  if (vm.count("config")) {
    args.config_file = vm["config"].as<std::string>();
  }

  if (vm.count("-S")) {
    args.s_args = vm["-S"].as<std::vector<std::string>>();
  }
  if (vm.count("-J")) {
    args.j_args = vm["-J"].as<std::vector<std::string>>();
  }

  return args;
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

/**
 * Load config file and change the passes list.
 * entry_data.json is a json file in the following format:
 * - apk_dir
 * - dex_list
 * - redex_options
 * - config
 * - jars
 */
Json::Value process_entry_data(const Json::Value& entry_data,
                               const Arguments& args) {
  Json::Value config_data =
      redex::parse_config(entry_data["config"].asString());
  // Change passes list in config data.
  config_data["redex"]["passes"] = Json::arrayValue;
  Json::Value& passes_list = config_data["redex"]["passes"];
  for (const std::string& pass_name : args.pass_names) {
    passes_list.append(pass_name);
  }
  int len = config_data["redex"]["passes"].size();
  if (len == 0 || passes_list[len - 1].asString() != "RegAllocPass") {
    passes_list.append("RegAllocPass");
  }

  // apk_dir
  if (entry_data.isMember("apk_dir")) {
    config_data["apk_dir"] = entry_data["apk_dir"].asString();
  }

  // Include -S and -J params.
  for (auto& key_value : args.s_args) {
    if (!add_value_to_config(config_data, key_value, false)) {
      std::cerr << "warning: cannot parse -S" << key_value << std::endl;
    }
  }
  for (auto& key_value : args.j_args) {
    if (!add_value_to_config(config_data, key_value, true)) {
      std::cerr << "warning: cannot parse -S" << key_value << std::endl;
    }
  }

  return config_data;
}
} // namespace

int main(int argc, char* argv[]) {
  Timer opt_timer("Redex-opt");
  Arguments args = parse_args(argc, argv);

  g_redex = new RedexContext();

  Json::Value entry_data;

  DexStoresVector stores;

  redex::load_all_intermediate(args.input_ir_dir, stores, &entry_data);

  // Set input dex magic to the first DexStore from the first dex file
  if (!stores.empty()) {
    auto first_dex_path = boost::filesystem::path(args.input_ir_dir) /
                          entry_data["dex_list"][0]["list"][0].asString();
    stores[0].set_dex_magic(load_dex_magic_from_dex(first_dex_path.c_str()));
  }

  if (!args.config_file.empty()) {
    entry_data["config"] = args.config_file;
  }

  args.redex_options.deserialize(entry_data);

  Json::Value config_data = process_entry_data(entry_data, args);
  ConfigFiles conf(config_data, args.output_ir_dir);

  const auto& passes = PassRegistry::get().get_passes();
  PassManager manager(passes, config_data, args.redex_options);
  manager.set_testing_mode();
  manager.run_passes(stores, conf);

  redex::write_all_intermediate(conf, args.output_ir_dir, args.redex_options,
                                stores, entry_data);

  delete g_redex;
  return 0;
}
