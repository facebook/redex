/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StaticIds.h"

#include <boost/filesystem/operations.hpp>
#include <boost/regex.hpp>
#include <fstream>
#include <functional>
#include <string>

namespace resources {
void read_static_ids_file(const std::string& path,
                          const std::function<void(const std::string& package,
                                                   const std::string& type,
                                                   const std::string& name,
                                                   uint32_t value)>& callback) {
  if (path.empty() || !boost::filesystem::exists(path)) {
    return;
  }
  std::ifstream input(path);
  if (!input) {
    fprintf(stderr, "[error] Can not open path %s\n", path.c_str());
    return;
  }

  std::string line;
  // com.facebook.packagename:string/flerp = 0x7f0a0123
  boost::regex expr{"^([^:]+):([^/]+)/([^ ]+) = 0x([0-9a-fA-F]+)$"};
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    boost::smatch what;
    if (boost::regex_search(line, what, expr)) {
      uint32_t id = std::stoul(what[4], nullptr, 16);
      callback(what[1], what[2], what[3], id);
    }
  }
}
} // namespace resources
