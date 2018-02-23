/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

struct QuickData {
  std::map<std::string, std::map<uint32_t, uint32_t>> dex_to_idx_to_offset;

  void add_idx_offset(const std::string& classes,
                      const uint32_t idx,
                      const uint32_t offset) {
    if (dex_to_idx_to_offset.count(classes) == 0) {
      std::map<uint32_t, uint32_t> dex_map;
      dex_to_idx_to_offset[classes] = std::move(dex_map);
    }
    dex_to_idx_to_offset[classes][idx] = offset;
  }

  void serialize(FILE* fd) {
    for (auto dex_to_map = dex_to_idx_to_offset.begin();
         dex_to_map != dex_to_idx_to_offset.end();
         ++dex_to_map) {
      fprintf(fd, "%s\n", dex_to_map->first.c_str());
      for (auto idx_to_offset = dex_to_map->second.begin();
           idx_to_offset != dex_to_map->second.end();
           ++idx_to_offset) {
        fprintf(fd, "%d:%d\n", idx_to_offset->first, idx_to_offset->second);
      }
    }
  }

  void deserialize(std::ifstream& istream) {
    std::string line;
    std::string classes;
    while (std::getline(istream, line)) {
      size_t found = line.find(":");
      if (found == std::string::npos) {
        std::map<uint32_t, uint32_t> dex_map;
        dex_to_idx_to_offset[line] = std::move(dex_map);
        classes = line;
      } else {
        dex_to_idx_to_offset[classes][std::stoul(line.substr(0, found))] =
            std::stoul(line.substr(found + 1));
      }
    }
    for (auto dex_to_map = dex_to_idx_to_offset.begin();
         dex_to_map != dex_to_idx_to_offset.end();
         ++dex_to_map) {
      printf("%s\n", dex_to_map->first.c_str());
      for (auto idx_to_offset = dex_to_map->second.begin();
           idx_to_offset != dex_to_map->second.end();
           ++idx_to_offset) {
        printf("%d:%d\n", idx_to_offset->first, idx_to_offset->second);
      }
    }
  }
};
