/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <fstream>
#include <iostream>
#include <json/json.h>

#include "DexClass.h"
#include "DexPosition.h"

DexPosition::DexPosition(DexString* file, uint32_t line)
  : file(file), line(line), parent(nullptr) {}

void RealPositionMapper::register_position(DexPosition* pos) {
  m_pos_line_map[pos] = -1;
}

uint32_t RealPositionMapper::get_line(DexPosition* pos) {
  return m_pos_line_map.at(pos) + 1;
}

uint32_t RealPositionMapper::position_to_line(DexPosition* pos) {
  auto idx = m_positions.size();
  m_positions.emplace_back(pos);
  m_pos_line_map[pos] = idx;
  return get_line(pos);
}

void RealPositionMapper::write_map() {
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
  std::ofstream ofs(m_filename.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  for (auto pos : m_positions) {
    auto parent_line = 0;
    try {
      parent_line = pos->parent == nullptr ? 0 : get_line(pos->parent);
    } catch (std::out_of_range& e) {
      std::cerr << "Parent position " << show(pos->parent) << " of "
                << show(pos) << " was not registered" << std::endl;
    }
    Json::Value json;
    json["file"] = std::string(pos->file->c_str());
    json["line"] = pos->line;
    json["parent"] = parent_line;
    ofs << Json::FastWriter().write(json);
  }
}

PositionMapper* PositionMapper::make(const std::string filename) {
  if (filename == "") {
    // If no path is provided for the map, just pass the original line numbers
    // through to the output. This does mean that the line numbers will be
    // incorrect for inlined code.
    return new NoopPositionMapper();
  } else {
    return new RealPositionMapper(filename);
  }
}

DexString* RealPositionMapper::get_source_file(const DexClass*) {
  // Note: When remapping line numbers, we don't simply emit DEX_NO_INDEX for
  // the source_file_idx because that would cause stack traces to print
  // "at com.foo.bar (Unknown source)" even when line number data is
  // available. So we make the source_file_idx point at an empty string
  // instead.
  return DexString::make_string("");
}

DexString* NoopPositionMapper::get_source_file(const DexClass* clz) {
  return clz->get_source_file();
}


uint32_t NoopPositionMapper::get_next_line(const DexDebugItem* dbg) {
  // XXX: we could be smarter and look for the first position entry
  return dbg->get_line_start();
}
