/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "PositionMap.h"

namespace {

constexpr uint32_t kMagic = 0xfaceb000;
constexpr uint32_t kVersion = 2;

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  out.insert(out.end(), reinterpret_cast<uint8_t*>(&v),
             reinterpret_cast<uint8_t*>(&v) + sizeof(v));
}

void put_string(std::vector<uint8_t>& out, const std::string& s) {
  put_u32(out, static_cast<uint32_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

void put_position(std::vector<uint8_t>& out, const PositionItem& item) {
  out.insert(out.end(), reinterpret_cast<const uint8_t*>(&item),
             reinterpret_cast<const uint8_t*>(&item) + sizeof(item));
}

// A well-formed two-entry map: strings {"LFoo;", "bar", "Foo.java"} and two
// positions, the second naming the first as its parent.
std::vector<uint8_t> well_formed_map() {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 3); // string count
  put_string(b, "LFoo;");
  put_string(b, "bar");
  put_string(b, "Foo.java");
  put_u32(b, 2); // position count
  put_position(b, PositionItem{0, 1, 2, 10, 0});
  put_position(b, PositionItem{0, 1, 2, 20, 1});
  return b;
}

// Writes `bytes` to a fresh file and returns its path. The file is removed by
// the fixture, so a failing assertion does not leave one behind.
class TempMap {
 public:
  explicit TempMap(const std::vector<uint8_t>& bytes) {
    std::string tmpl = "/tmp/redex-position-map-XXXXXX";
    int fd = mkstemp(tmpl.data());
    EXPECT_NE(fd, -1);
    if (fd == -1) {
      // A helper cannot ASSERT, and continuing would close(-1) and unlink the
      // unsubstituted template. `m_path` stays empty, so the destructor's
      // unlink is a no-op and the failure above is what the test reports.
      return;
    }
    m_path = tmpl;
    if (!bytes.empty()) {
      EXPECT_EQ(write(fd, bytes.data(), bytes.size()),
                static_cast<ssize_t>(bytes.size()));
    }
    close(fd);
  }
  ~TempMap() { unlink(m_path.c_str()); }
  const char* path() const { return m_path.c_str(); }

 private:
  std::string m_path;
};

} // namespace

// Positive control. Without this, every assertion below could pass because the
// builder emits something `read_map` rejects for an unrelated reason.
TEST(PositionMapReaderTest, ReadsAWellFormedMap) {
  TempMap f(well_formed_map());
  auto map = read_map(f.path());
  ASSERT_NE(map, nullptr);
  EXPECT_EQ(map->string_pool.size(), 3);
  EXPECT_EQ(map->positions_size, 2);

  auto stack = get_stack(*map, 1);
  ASSERT_EQ(stack.size(), 2);
  EXPECT_EQ(stack[0].cls, "LFoo;");
  EXPECT_EQ(stack[0].method, "bar");
  EXPECT_EQ(stack[0].filename, "Foo.java");
  EXPECT_EQ(stack[0].line, 20);
  EXPECT_EQ(stack[1].line, 10);
}

// The reader takes the file from storage, not from the process that wrote it,
// so a truncated one is the ordinary result of an interrupted upload.
// Truncating at every length must yield null rather than a read past the
// mapping.
TEST(PositionMapReaderTest, RejectsTruncationAtEveryLength) {
  const auto full = well_formed_map();
  for (size_t len = 1; len < full.size(); ++len) {
    std::vector<uint8_t> truncated(
        full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
    TempMap f(truncated);
    EXPECT_EQ(read_map(f.path()), nullptr)
        << "accepted a map truncated to " << len << " of " << full.size()
        << " bytes";
  }
}

// The worst single case, called out separately because it is the one that read
// past the end of the mapping: the count is honoured with no bytes behind it.
TEST(PositionMapReaderTest, RejectsAPositionCountLargerThanTheFile) {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 0); // no strings
  put_u32(b, 0xffffff); // claims ~16M positions, supplies none
  TempMap f(b);
  EXPECT_EQ(read_map(f.path()), nullptr);
}

TEST(PositionMapReaderTest, RejectsAStringSizeLargerThanTheFile) {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 1); // one string
  put_u32(b, 0xffffff); // claiming ~16M bytes, supplying none
  TempMap f(b);
  EXPECT_EQ(read_map(f.path()), nullptr);
}

TEST(PositionMapReaderTest, RejectsEmptyMissingAndMalformedFiles) {
  EXPECT_EQ(read_map("/nonexistent/definitely-not-a-line-map"), nullptr);

  TempMap empty(std::vector<uint8_t>{});
  EXPECT_EQ(read_map(empty.path()), nullptr);

  auto bad_magic = well_formed_map();
  bad_magic[0] ^= 0xff;
  TempMap bm(bad_magic);
  EXPECT_EQ(read_map(bm.path()), nullptr);

  auto bad_version = well_formed_map();
  bad_version[sizeof(uint32_t)] = 0x7f;
  TempMap bv(bad_version);
  EXPECT_EQ(read_map(bv.path()), nullptr);
}

// String ids come from the file, and `get_stack` is not the only thing that
// indexes the pool with them -- `linemapdump` does it straight from a
// `PositionItem`. So the map must not be handed out at all.
TEST(PositionMapReaderTest, RejectsAStringIdOutsideThePool) {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 1);
  put_string(b, "LFoo;");
  put_u32(b, 1);
  put_position(b, PositionItem{99, 99, 99, 1, 0}); // ids far past the pool
  TempMap f(b);
  EXPECT_EQ(read_map(f.path()), nullptr);
}

// One id per field, so a check covering only `class_id` cannot pass this.
TEST(PositionMapReaderTest, RejectsAStringIdOutsideThePoolInAnyField) {
  for (int field = 0; field < 3; ++field) {
    PositionItem item{0, 0, 0, 1, 0};
    (field == 0   ? item.class_id
     : field == 1 ? item.method_id
                  : item.file_id) = 99;
    std::vector<uint8_t> b;
    put_u32(b, kMagic);
    put_u32(b, kVersion);
    put_u32(b, 1);
    put_string(b, "LFoo;");
    put_u32(b, 1);
    put_position(b, item);
    TempMap f(b);
    EXPECT_EQ(read_map(f.path()), nullptr)
        << "accepted a bad id in field " << field;
  }
}

// `parent` is a one-based index into the same table, so anything above the
// position count would be followed off the end of it.
TEST(PositionMapReaderTest, RejectsAParentOutsideTheTable) {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 3);
  put_string(b, "LFoo;");
  put_string(b, "bar");
  put_string(b, "Foo.java");
  put_u32(b, 1);
  put_position(b, PositionItem{0, 1, 2, 10, 7}); // one position, parent 7
  TempMap f(b);
  EXPECT_EQ(read_map(f.path()), nullptr);
}

// A cycle is the one hazard the per-item validation in `read_map` cannot see:
// every hop of `0 -> 1 -> 0` is individually within the table, so the map is
// accepted and the walk itself has to terminate.
TEST(PositionMapReaderTest, TerminatesOnACyclicParentChain) {
  std::vector<uint8_t> b;
  put_u32(b, kMagic);
  put_u32(b, kVersion);
  put_u32(b, 3);
  put_string(b, "LFoo;");
  put_string(b, "bar");
  put_string(b, "Foo.java");
  put_u32(b, 2);
  // 0's parent is 2 -> index 1; 1's parent is 1 -> index 0. A two-cycle.
  put_position(b, PositionItem{0, 1, 2, 10, 2});
  put_position(b, PositionItem{0, 1, 2, 20, 1});
  TempMap f(b);
  auto map = read_map(f.path());
  ASSERT_NE(map, nullptr);
  // Exactly the table size, not one more: no legitimate chain can revisit an
  // entry, so a walk that emitted an extra frame would be reporting a stack
  // depth the map cannot represent.
  EXPECT_LE(get_stack(*map, 0).size(), map->positions_size);
}
