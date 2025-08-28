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

#include "androidfw/ResourceTypes.h"
#include "utils/Visitor.h"

namespace {
void run(int argc, char** argv) {
  namespace po = boost::program_options;
  po::options_description desc(
      "Allowed options. Choose one of --arsc or --xml");
  desc.add_options()("help,h", "print this help message");
  desc.add_options()("arsc", po::value<std::string>(), "path to an arsc file");
  desc.add_options()("xml", po::value<std::string>(), "path to xml file");

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    exit(EXIT_FAILURE);
  }

  if (vm.count("help") != 0u) {
    std::cout << desc << "\n";
    exit(EXIT_SUCCESS);
  }
  auto arsc_count = vm.count("arsc");
  auto arg_count = arsc_count + vm.count("xml");
  if (arg_count == 0 || arg_count == 2) {
    std::cerr << desc << "\n";
    exit(EXIT_FAILURE);
  }

  bool success{false};
  auto mode = (std::ios_base::openmode)(std::ios_base::in);
  if (arsc_count != 0u) {
    auto arsc_path = vm["arsc"].as<std::string>();
    auto map = std::make_unique<boost::iostreams::mapped_file>();
    map->open(arsc_path, mode);
    if (!map->is_open()) {
      std::cerr << "Could not map " << arsc_path << std::endl;
      exit(EXIT_FAILURE);
    }
    arsc::ResourceTableVisitor visitor;
    success = visitor.visit((void*)map->const_data(), map->size());
  } else {
    auto xml_path = vm["xml"].as<std::string>();
    auto map = std::make_unique<boost::iostreams::mapped_file>();
    map->open(xml_path, mode);
    if (!map->is_open()) {
      std::cerr << "Could not map " << xml_path << std::endl;
      exit(EXIT_FAILURE);
    }
    arsc::XmlFileVisitor visitor;
    success = visitor.visit((void*)map->const_data(), map->size());
  }

  if (!success) {
    std::cerr << "Could not parse file!" << std::endl;
    exit(EXIT_FAILURE);
  }
}
} // namespace

// This tool accepts an .arsc file or .xml file and parses it. It is meant to be
// built with libresource setting preprocessor flags to turn on verbose offset
// logging. Helpful for understanding where various structs lie within a large
// file.
int main(int argc, char** argv) {
  try {
    run(argc, argv);
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl << std::endl;
    exit(EXIT_FAILURE);
  }
  return 0;
}
