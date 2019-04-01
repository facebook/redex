/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <iostream>

#include "DexClass.h"
#include "DexLoader.h"
#include "PassRegistry.h"
#include "Timer.h"
#include "ToolsCommon.h"

namespace {

struct Arguments {
  std::string input_ir_dir;
  std::string output_ir_dir;
  std::vector<std::string> pass_names;
  RedexOptions redex_options;
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
  boost::filesystem::create_directory(args.output_ir_dir);

  if (vm.count("pass-name")) {
    args.pass_names = vm["pass-name"].as<std::vector<std::string>>();
  }

  return args;
}

/**
 * Process entry_data : Load config file and change the passes list
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
  if (len > 0 && passes_list[len - 1].asString() != "RegAllocPass") {
    passes_list.append("RegAllocPass");
  }

  // apk_dir
  if (entry_data.isMember("apk_dir")) {
    config_data["apk_dir"] = entry_data["apk_dir"].asString();
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
  if (stores.size() > 0) {
    auto first_dex_path = boost::filesystem::path(args.input_ir_dir) /
                          entry_data["dex_list"][0]["list"][0].asString();
    stores[0].set_dex_magic(load_dex_magic_from_dex(first_dex_path.c_str()));
  }

  args.redex_options.deserialize(entry_data);

  Json::Value config_data = process_entry_data(entry_data, args);
  ConfigFiles cfg(std::move(config_data), args.output_ir_dir);

  const auto& passes = PassRegistry::get().get_passes();
  PassManager manager(passes, config_data, args.redex_options);
  manager.set_testing_mode();
  manager.run_passes(stores, cfg);

  redex::write_all_intermediate(cfg, args.output_ir_dir, args.redex_options,
                                stores, entry_data);

  delete g_redex;
  return 0;
}
