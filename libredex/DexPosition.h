/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class DexClass;
class DexMethod;
class DexString;
class DexDebugItem;

struct DexPosition final {
  DexString* method{nullptr};
  DexString* file{nullptr};
  uint32_t line;
  // when a function gets inlined for the first time, all its DexPositions will
  // have the DexPosition of the callsite as their parent.
  DexPosition* parent{nullptr};
  DexPosition(uint32_t line);
  DexPosition(DexString* method, DexString* file, uint32_t line);

  void bind(DexString* method_, DexString* file_);
  bool operator==(const DexPosition&) const;
};

class PositionMapper {
 public:
  virtual ~PositionMapper() {};
  virtual DexString* get_source_file(const DexClass*) = 0;
  virtual uint32_t position_to_line(DexPosition*) = 0;
  virtual void register_position(DexPosition* pos) = 0;
  virtual void write_map() = 0;
  static PositionMapper* make(const std::string& map_filename,
                              const std::string& map_filename_v2);
};

/*
 * This allows us to recover the original file names and line numbers from
 * runtime stack traces of Dex files that have undergone inlining. The
 * PositionMapper produces a text file with this data, and the line numbers in
 * the Dex debug info indicate the line in this text file at which the real
 * position can be found.
 */
class RealPositionMapper : public PositionMapper {
  std::string m_filename;
  std::string m_filename_v2;
  std::vector<DexPosition*> m_positions;
  std::unordered_map<DexPosition*, int64_t> m_pos_line_map;
 protected:
  uint32_t get_line(DexPosition*);
  void write_map_v1();
  void write_map_v2();
 public:
  RealPositionMapper(const std::string& filename,
                     const std::string& filename_v2)
      : m_filename(filename), m_filename_v2(filename_v2) {}
  DexString* get_source_file(const DexClass*) override;
  uint32_t position_to_line(DexPosition*) override;
  void register_position(DexPosition* pos) override;
  void write_map() override;
};

class NoopPositionMapper : public PositionMapper {
 public:
  DexString* get_source_file(const DexClass*) override;
  uint32_t position_to_line(DexPosition* pos) override { return pos->line; }
  void register_position(DexPosition* pos) override {}
  void write_map() override {}
};
