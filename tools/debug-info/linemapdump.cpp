/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>

#include "PositionMap.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: linemapdump mapping_file\n";
    abort();
  }
  auto map = read_map(argv[1]);
  for (size_t i = 0; i < map->positions_size; ++i) {
    auto pi = map->positions[i];
    std::cout << map->string_pool[pi.class_id] << "."
              << map->string_pool[pi.method_id] << map->string_pool[pi.file_id]
              << ":" << pi.line << " => " << pi.parent << std::endl;
  }
}
