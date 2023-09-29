/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/program_options.hpp>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <memory>
#include <secure_lib/secure_string.h>
#include <sstream>

#include "ArscStats.h"
#include "Debug.h"

namespace {
inline std::string csv_escape(std::string value) {
  always_assert_log(value.find('\n') == std::string::npos,
                    "not supporting new lines");
  if (value.find(',') != std::string::npos ||
      value.find('\"') != std::string::npos) {
    boost::replace_all(value, "\"", "\"\"");
    return "\"" + value + "\"";
  }
  return value;
}

void print_csv(const std::vector<attribution::Result>& results,
               bool hide_uninteresting) {
  std::cout << "ID,Type,Name,Private Size,Shared Size,Proportional Size,Config "
               "Count,Configs"
            << std::endl;
  for (const auto& result : results) {
    if (hide_uninteresting && result.sizes.proportional_size == 0) {
      continue;
    }
    auto joined_configs = boost::algorithm::join(result.configs, " ");
    std::cout << "0x" << std::hex << result.id << std::dec << ","
              << csv_escape(result.type) << "," << csv_escape(result.name)
              << "," << result.sizes.private_size << ","
              << result.sizes.shared_size << ","
              << result.sizes.proportional_size << "," << result.configs.size()
              << "," << csv_escape(joined_configs) << std::endl;
  }
}

bool read_rename_map(const std::string& resid_to_name_path,
                     attribution::ResourceNames* out) {
  std::ifstream deob_file(resid_to_name_path, std::ifstream::binary);
  Json::Reader reader;
  Json::Value root;
  if (!reader.parse(deob_file, root)) {
    std::cerr << reader.getFormatedErrorMessages() << std::endl;
    return false;
  }
  for (Json::ValueIterator it = root.begin(); it != root.end(); ++it) {
    std::string hex_str(it.key().asCString());
    uint32_t id = std::stoul(hex_str, nullptr, 16);
    std::string name(it->asCString());
    out->emplace(id, name);
  }
  return true;
}

void do_attribution(const void* data,
                    size_t len,
                    bool hide_uninteresting,
                    const attribution::ResourceNames& given_resid_to_name) {
  attribution::ArscStats stats(data, len, given_resid_to_name);
  print_csv(stats.compute(), hide_uninteresting);
}

void run(int argc, char** argv) {
  namespace po = boost::program_options;
  po::options_description desc("Allowed options");
  desc.add_options()("help,h", "print this help message");
  desc.add_options()(
      "file", po::value<std::string>(), "required path to arsc file");
  desc.add_options()("resid",
                     po::value<std::string>(),
                     "optional path to resource id to name json file");
  desc.add_options()("hide-uninteresting",
                     po::bool_switch(),
                     "suppress resource ids that are empty");

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    exit(EXIT_FAILURE);
  }

  if (vm.count("help")) {
    std::cout << desc << "\n";
    exit(EXIT_SUCCESS);
  }
  if (!vm.count("file")) {
    std::cerr << desc << "\n";
    exit(EXIT_FAILURE);
  }

  attribution::ResourceNames resid_to_name;
  if (vm.count("resid")) {
    auto resid_to_name_path = vm["resid"].as<std::string>();
    if (!read_rename_map(resid_to_name_path, &resid_to_name)) {
      std::cerr << "Failed to parse resid to name json file: "
                << resid_to_name_path << std::endl;
      exit(EXIT_FAILURE);
    }
  }
  auto arsc_path = vm["file"].as<std::string>();
  auto map = std::make_unique<boost::iostreams::mapped_file>();
  auto mode = (std::ios_base::openmode)(std::ios_base::in);
  map->open(arsc_path, mode);
  if (!map->is_open()) {
    std::cerr << "Could not map " << arsc_path << std::endl;
    exit(EXIT_FAILURE);
  }
  do_attribution(map->const_data(),
                 map->size(),
                 vm["hide-uninteresting"].as<bool>(),
                 resid_to_name);
}
} // namespace

// This tool accepts an .arsc file and spits out a csv of useful stats on how
// much space is being taken up by what resource id. It aims to be similar in
// concept to https://github.com/google/android-arscblamer but operate
// differently to:
// 1) handle arsc files that have been obfuscated, apply a deobfuscation map.
// 2) be able to traverse files that have been mangled substantially by
//    deduplication, canonical offsets, etc.
// 3) handle shared data in a more sensible way (it seems more intuitive to
//    count type string data as overhead, not shared data).
int main(int argc, char** argv) {
  try {
    run(argc, argv);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    exit(EXIT_FAILURE);
  }
  return 0;
}
