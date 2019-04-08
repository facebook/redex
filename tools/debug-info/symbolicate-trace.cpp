/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/regex.hpp>
#include <iostream>
#include <string>

#include "PositionMap.h"

boost::regex trace_regex(R"/(((\s+at\s+)[^(]*)\(:(\d+)\)\s?)/");

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: cat trace | remap mapping_file\n";
    abort();
  }
  auto map = read_map(argv[1]);
  for (std::string line; std::getline(std::cin, line);) {
    boost::smatch matches;
    if (boost::regex_match(line, matches, trace_regex)) {
      auto idx = std::stoi(matches[3]) - 1;
      auto stack = get_stack(*map, idx);
      for (auto pos : stack) {
        std::cout << matches[2] << pos.cls << "." << pos.method << "("
                  << pos.filename << ":" << pos.line << ")" << std::endl;
      }
    } else {
      std::cout << line << std::endl;
    }
  }
}
