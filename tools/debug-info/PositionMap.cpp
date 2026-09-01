/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <array>
#include <bit>
#include <boost/scope_exit.hpp>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

#include "PositionMap.h"

std::unique_ptr<PositionMap> read_map(const char* filename) {
  int fd = open(filename, O_RDONLY);
  if (fd == -1) {
    std::cerr << "open failed for file (" << filename
              << ") with error: " << strerror(errno) << '\n';
    return nullptr;
  }
  BOOST_SCOPE_EXIT_ALL(=) { close(fd); };

  struct stat buf;
  if (fstat(fd, &buf) != 0) {
    std::cerr << "Cannot fstat file (" << filename
              << ") with error: " << strerror(errno) << '\n';
    return nullptr;
  }
  if (buf.st_size <= 0) {
    std::cerr << "Line map file (" << filename << ") is empty\n";
    return nullptr;
  }
  const size_t size = static_cast<size_t>(buf.st_size);

  void* base = mmap(nullptr, size, PROT_READ, MAP_FILE | MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    std::cerr << "mmap failed for file (" << filename
              << ") with error: " << strerror(errno) << '\n';
    return nullptr;
  }
  // Unmaps what mmap returned. Advancing a cursor instead of the base pointer
  // is what makes that possible: munmap wants a page-aligned address, so
  // handing it a pointer part-way into the mapping fails and leaks it.
  BOOST_SCOPE_EXIT_ALL(=) { munmap(base, size); };

  // Every count in the file drives a read, and the file is an input we did not
  // produce -- a truncated one is the ordinary case after a failed upload. Each
  // read is therefore bounded against the mapping rather than trusted.
  const uint8_t* cursor = static_cast<const uint8_t*>(base);
  const uint8_t* const end = cursor + size;
  auto has_room = [&](size_t bytes) {
    return static_cast<size_t>(end - cursor) >= bytes;
  };
  // Assemble through a byte array rather than reading a uint32_t out of the
  // mapping directly: nothing guarantees these offsets are aligned.
  // std::bit_cast rather than memcpy because this file is also built by the OSS
  // makefiles, which have no secure_lib, so the try_checked_memcpy the security
  // linter asks for is not available here. The bound is the has_room check
  // above.
  auto read_u32 = [&](uint32_t* out) {
    if (!has_room(sizeof(uint32_t))) {
      return false;
    }
    std::array<uint8_t, sizeof(uint32_t)> bytes{};
    std::copy(cursor, cursor + sizeof(uint32_t), bytes.begin());
    *out = std::bit_cast<uint32_t>(bytes);
    cursor += sizeof(uint32_t);
    return true;
  };

  uint32_t magic;
  if (!read_u32(&magic)) {
    std::cerr << "Line map is too short for a header\n";
    return nullptr;
  }
  if (magic != 0xfaceb000) {
    std::cerr << "Magic number mismatch\n";
    return nullptr;
  }
  uint32_t version;
  if (!read_u32(&version)) {
    std::cerr << "Line map is too short for a header\n";
    return nullptr;
  }
  if (version != 2) {
    std::cerr << "Version mismatch\n";
    return nullptr;
  }

  std::unique_ptr<PositionMap> map(new PositionMap());
  uint32_t spool_count;
  if (!read_u32(&spool_count)) {
    std::cerr << "Line map has no string pool count\n";
    return nullptr;
  }
  // No reserve on `spool_count`: it comes from the file, and a four-byte file
  // claiming four billion strings would otherwise ask for the allocation before
  // anything has checked the strings are there.
  for (uint32_t i = 0; i < spool_count; ++i) {
    uint32_t ssize;
    if (!read_u32(&ssize) || !has_room(ssize)) {
      std::cerr << "Truncated line map string pool at entry " << i << " of "
                << spool_count << '\n';
      return nullptr;
    }
    map->string_pool.emplace_back(reinterpret_cast<const char*>(cursor), ssize);
    cursor += ssize;
  }

  uint32_t pos_count;
  if (!read_u32(&pos_count)) {
    std::cerr << "Line map has no positions count\n";
    return nullptr;
  }
  if (!has_room(static_cast<size_t>(pos_count) * sizeof(PositionItem))) {
    std::cerr << "Could not find all " << pos_count
              << " position items. Is the line map truncated?" << '\n';
    return nullptr;
  }
  map->positions.reset(new PositionItem[pos_count]);
  map->positions_size = pos_count;
  // Byte-wise copy for the same reason as read_u32 above: no secure_lib in the
  // OSS build, so no try_checked_memcpy. Both ends are bounded -- the source by
  // the has_room check above, the destination by having just been allocated at
  // exactly this size.
  static_assert(std::is_trivially_copyable_v<PositionItem>,
                "PositionItem is copied as raw bytes out of the mapping");
  std::copy(cursor,
            cursor + static_cast<size_t>(pos_count) * sizeof(PositionItem),
            reinterpret_cast<uint8_t*>(map->positions.get()));

  // Every id in a position item comes from the file, so the whole table is
  // validated once here rather than at each use. `get_stack` is not the only
  // consumer -- `linemapdump` indexes the string pool straight out of a
  // `PositionItem` -- and a check placed at one use site is a check the next
  // consumer does not get. Callers may therefore treat the ids of a map that
  // was returned as in range.
  const size_t spool_size = map->string_pool.size();
  for (uint32_t i = 0; i < pos_count; ++i) {
    const PositionItem pi = map->positions[i];
    if (pi.class_id >= spool_size || pi.method_id >= spool_size ||
        pi.file_id >= spool_size) {
      std::cerr << "Line map position " << i
                << " names a string outside the pool\n";
      return nullptr;
    }
    // `parent` is a one-based index into the same table, zero meaning none.
    if (pi.parent > pos_count) {
      std::cerr << "Line map position " << i
                << " names a parent outside the table\n";
      return nullptr;
    }
  }
  return map;
}

std::vector<Position> get_stack(const PositionMap& map, int64_t idx) {
  std::vector<Position> stack;
  // `read_map` has range-checked every id in the table, but a cycle is not a
  // property of any one item: each hop of `0 -> 1 -> 0` is individually in
  // range. It is a property of the walk, so it is bounded here. A legitimate
  // chain cannot be longer than the table, which bounds it without tracking
  // what has been visited. The index bound stays because `idx` is the caller's,
  // parsed out of a stack trace rather than read from the map.
  for (size_t steps = 0; idx >= 0 && (size_t)idx < map.positions_size &&
                         steps < map.positions_size;
       ++steps) {
    auto pi = map.positions[idx];
    // Copied out of the packed struct first: `emplace_back` binds its
    // arguments by reference, and a reference cannot bind to a packed field --
    // gcc rejects it outright where clang accepts it. This file is compiled by
    // the OSS makefiles with gcc, so the copy is what keeps that build working.
    const uint32_t line = pi.line;
    stack.emplace_back(map.string_pool[pi.class_id],
                       map.string_pool[pi.method_id],
                       map.string_pool[pi.file_id],
                       line);
    idx = (int64_t)pi.parent - 1;
  }
  return stack;
}
