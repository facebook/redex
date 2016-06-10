/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <iostream>

#include "DexClass.h"
#include "DexPosition.h"

DexPosition::DexPosition(DexString* file, uint32_t line)
  : file(file), line(line), parent(nullptr) {}

void PositionMapper::register_position(DexPosition* pos) {
  m_pos_line_map[pos] = -1;
}

uint32_t PositionMapper::get_line(DexPosition* pos) {
  return m_pos_line_map.at(pos) + 1;
}

uint32_t PositionMapper::position_to_line(DexPosition* pos) {
  auto idx = m_positions.size();
  m_positions.emplace_back(pos);
  m_pos_line_map[pos] = idx;
  return get_line(pos);
}

void PositionMapper::write_to(const char* filename) {
  if (strcmp(filename, "") == 0) {
    return;
  }
  // to ensure that the line numbers in the Dex are as compact as possible,
  // we put the emitted positions at the start of the list and rest at the end
  for (auto item : m_pos_line_map) {
    auto line = item.second;
    if (line == -1) {
      auto idx = m_positions.size();
      m_positions.emplace_back(item.first);
      m_pos_line_map[item.first] = idx;
    }
  }
  FILE* fp = fopen(filename, "w");
  for (auto pos : m_positions) {
    auto parent_line = 0;
    try {
      parent_line = pos->parent == nullptr ? 0 : get_line(pos->parent);
    } catch (std::out_of_range& e) {
      std::cerr << "Parent position " << show(pos->parent) << " of "
        << show(pos) << " was not registered" << std::endl;
    }
    fprintf(fp, "%s:%d|%d\n", pos->file->c_str(), pos->line, parent_line);
  }
  fclose(fp);
}
