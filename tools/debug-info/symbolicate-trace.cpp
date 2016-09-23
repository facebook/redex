/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/regex.hpp>
#include <boost/scope_exit.hpp>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
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

std::unique_ptr<PositionMap> read_map(const char* filename) {
  int fd = open(filename, O_RDONLY);
  if (fd == -1) {
    std::cerr << "open failed for file (" << filename
              << ") with error: " << strerror(errno) << std::endl;
    return nullptr;
  }
  struct stat buf;
  if (fstat(fd, &buf)) {
    std::cerr << "Cannot fstat file (" << filename
              << ") with error: " << strerror(errno) << std::endl;
    return nullptr;
  }
  uint8_t* mapping = (uint8_t*)mmap(
      nullptr, buf.st_size, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
  uint32_t magic = *(uint32_t*)mapping;
  mapping += sizeof(uint32_t);
  if (mapping == MAP_FAILED) {
    std::cerr << "mmap failed for file (" << filename
              << ") with error: " << strerror(errno) << std::endl;
    return nullptr;
  }
  BOOST_SCOPE_EXIT_ALL(=, &buf) {
    munmap(mapping, buf.st_size);
  };
  if (magic != 0xfaceb000) {
    std::cerr << "Magic number mismatch\n";
    return nullptr;
  }
  uint32_t version = *(uint32_t*)mapping;
  mapping += sizeof(uint32_t);
  if (version != 1) {
    std::cerr << "Version mismatch\n";
    return nullptr;
  }

  std::unique_ptr<PositionMap> map(new PositionMap());
  std::vector<std::string> string_pool;
  uint32_t spool_count = *(uint32_t*)mapping;
  mapping += sizeof(uint32_t);
  for (uint32_t i = 0; i < spool_count; ++i) {
    uint32_t ssize = *(uint32_t*)mapping;
    mapping += sizeof(uint32_t);
    map->string_pool.emplace_back((const char*)mapping, ssize);
    mapping += ssize;
  }
  uint32_t pos_count = *(uint32_t*)mapping;
  mapping += sizeof(uint32_t);
  map->positions.reset(new PositionItem[pos_count]);
  memcpy(map->positions.get(), mapping, pos_count * sizeof(PositionItem));
  return map;
}

std::vector<Position> get_stack(const PositionMap& map, int64_t idx) {
  std::vector<Position> stack;
  while (idx >= 0 && (size_t)idx < map.positions_size) {
    auto pi = map.positions[idx];
    stack.push_back(Position(map.string_pool[pi.file_id], pi.line));
    idx = (int64_t)pi.parent - 1;
  }
  return stack;
}

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
