/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <stdint.h>
#include <vector>
#include <unordered_map>

class DexString;

struct DexPosition final {
  DexString* file;
  uint32_t line;
  // when a function gets inlined for the first time, all its DexPositions will
  // have the DexPosition of the callsite as their parent.
  DexPosition* parent;
  DexPosition(DexString* file, uint32_t line);
};

/*
 * This allows us to recover the original file names and line numbers from
 * runtime stack traces of Dex files that have undergone inlining. The
 * PositionMapper produces a text file with this data, and the line numbers in
 * the Dex debug info indicate the line in this text file at which the real
 * position can be found.
 */
class PositionMapper final {
  std::vector<DexPosition*> m_positions;
  std::unordered_map<DexPosition*, int64_t> m_pos_line_map;
 public:
  uint32_t position_to_line(DexPosition*);
  uint32_t get_next_line() {
    // line numbers are not allowed to be less than one
    return m_positions.size() + 1;
  }
  void register_position(DexPosition* pos);
  uint32_t get_line(DexPosition* pos);

  void write_to(const char* filename);
};
