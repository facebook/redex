/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <string>
#include <vector>

struct __attribute__((packed)) PositionItem {
  uint32_t class_id;
  uint32_t method_id;
  uint32_t file_id;
  uint32_t line;
  uint32_t parent;
};

struct Position {
  std::string cls;
  std::string method;
  std::string filename;
  uint32_t line;
  Position(const std::string& cls,
           const std::string& method,
           const std::string& filename,
           uint32_t line)
      : cls(cls), method(method), filename(filename), line(line) {}
};

struct PositionMap {
  std::vector<std::string> string_pool;
  std::unique_ptr<PositionItem[]> positions;
  size_t positions_size;
};

std::unique_ptr<PositionMap> read_map(const char* filename);
std::vector<Position> get_stack(const PositionMap& map, int64_t idx);
