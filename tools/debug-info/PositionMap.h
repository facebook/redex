/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */


#include <string>
#include <vector>

struct __attribute__((packed)) PositionItem {
  uint32_t file_id;
  uint32_t line;
  uint32_t parent;
};

struct Position {
  std::string filename;
  uint32_t line;
  Position(const std::string& filename, uint32_t line)
      : filename(filename), line(line) {}
};

struct PositionMap {
  std::vector<std::string> string_pool;
  std::unique_ptr<PositionItem[]> positions;
  size_t positions_size;
};

std::unique_ptr<PositionMap> read_map(const char* filename);
std::vector<Position> get_stack(const PositionMap& map, int64_t idx);
