/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class DexString;
class DexDebugItem;

struct DexPosition final {
  DexString* file;
  uint32_t line;
  // when a function gets inlined for the first time, all its DexPositions will
  // have the DexPosition of the callsite as their parent.
  DexPosition* parent;
  DexPosition(DexString* file, uint32_t line);
};

class PositionMapper {
 public:
  virtual DexString* get_source_file(const DexClass*) = 0;
  virtual uint32_t position_to_line(DexPosition*) = 0;
  virtual uint32_t get_next_line(const DexDebugItem*) = 0;
  virtual void register_position(DexPosition* pos) = 0;
  virtual void write_map() = 0;
  static PositionMapper* make(const std::string filename);
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
  std::vector<DexPosition*> m_positions;
  std::unordered_map<DexPosition*, int64_t> m_pos_line_map;
 protected:
  uint32_t get_line(DexPosition*);
 public:
  RealPositionMapper(const std::string filename): m_filename(filename) {}
  virtual DexString* get_source_file(const DexClass*);
  virtual uint32_t position_to_line(DexPosition*);
  virtual uint32_t get_next_line(const DexDebugItem*) {
    // line numbers are not allowed to be less than one
    return m_positions.size() + 1;
  }
  virtual void register_position(DexPosition* pos);
  virtual void write_map();
};

class NoopPositionMapper : public PositionMapper {
 public:
  virtual DexString* get_source_file(const DexClass*);
  virtual uint32_t position_to_line(DexPosition* pos) {
    return pos->line;
  }
  virtual uint32_t get_next_line(const DexDebugItem*);
  virtual void register_position(DexPosition* pos) {}
  virtual void write_map() {}
};
