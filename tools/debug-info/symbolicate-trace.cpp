/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/regex.hpp>
#include <iostream>
#include <string>

#include "PositionMap.h"

boost::regex trace_regex(R"/((\s+at\s+[^(]*)\(:(\d+)\)\s?)/");

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: cat trace | remap mapping_file\n";
    abort();
  }
  auto map = read_map(argv[1]);
  for (std::string line; std::getline(std::cin, line);) {
    boost::smatch matches;
    if (boost::regex_match(line, matches, trace_regex)) {
      auto idx = std::stoi(matches[2]) - 1;
      auto stack = get_stack(*map, idx);
      for (auto pos : stack) {
        std::cout << matches[1] << "(" << pos.filename << ":" << pos.line
                  << ")" << std::endl;
      }
    } else {
      std::cout << line << std::endl;
    }
  }
}
