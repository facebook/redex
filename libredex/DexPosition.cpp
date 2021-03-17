/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>

#include "DexClass.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "Show.h"

DexPosition::DexPosition(uint32_t line) : line(line) {}

DexPosition::DexPosition(DexString* method, DexString* file, uint32_t line)
    : method(method), file(file), line(line) {}

void DexPosition::bind(DexString* method_, DexString* file_) {
  this->method = method_;
  this->file = file_;
}

bool DexPosition::operator==(const DexPosition& that) const {
  return method == that.method && file == that.file && line == that.line &&
         (parent == that.parent ||
          (parent != nullptr && that.parent != nullptr &&
           *parent == *that.parent));
}

std::unique_ptr<DexPosition> DexPosition::make_synthetic_entry_position(
    const DexMethod* method) {
  auto method_str = DexString::make_string(show_deobfuscated(method));

  // For source, see if the class has a source.
  DexString* source = nullptr;
  auto cls = type_class(method->get_class());
  if (cls != nullptr) {
    source = cls->get_source_file();
  }
  // Fall back to "UnknownSource".
  if (source == nullptr) {
    source = DexString::make_string("UnknownSource");
  }

  return std::make_unique<DexPosition>(method_str, source, 0);
}

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
  if (!m_filename_v2.empty()) {
    write_map_v2();
  }
}

void RealPositionMapper::write_map_v2() {
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
  /*
   * Map file layout:
   * 0xfaceb000 (magic number)
   * version (4 bytes)
   * string_pool_size (4 bytes)
   * string_pool[string_pool_size]
   * positions_size (4 bytes)
   * positions[positions_size]
   *
   * Each member of the string pool is encoded as follows:
   * string_length (4 bytes)
   * char[string_length]
   */
  std::ostringstream pos_out;
  std::unordered_map<std::string, uint32_t> string_ids;
  std::vector<std::string> string_pool;

  auto id_of_string = [&](const std::string& s) -> uint32_t {
    if (string_ids.find(s) == string_ids.end()) {
      string_ids[s] = string_pool.size();
      string_pool.push_back(s);
    }
    return string_ids.at(s);
  };

  for (auto pos : m_positions) {
    uint32_t parent_line = 0;
    try {
      parent_line = pos->parent == nullptr ? 0 : get_line(pos->parent);
    } catch (std::out_of_range& e) {
      std::cerr << "Parent position " << show(pos->parent) << " of "
                << show(pos) << " was not registered" << std::endl;
    }
    // of the form "class_name.method_name:(arg_types)return_type"
    const auto& full_method_name = pos->method->str();
    // strip out the args and return type
    auto qualified_method_name =
        full_method_name.substr(0, full_method_name.find(':'));
    auto class_name = java_names::internal_to_external(
        qualified_method_name.substr(0, qualified_method_name.rfind('.')));
    auto method_name =
        qualified_method_name.substr(qualified_method_name.rfind('.') + 1);
    auto class_id = id_of_string(class_name);
    auto method_id = id_of_string(method_name);
    auto file_id = id_of_string(pos->file->c_str());
    pos_out.write((const char*)&class_id, sizeof(class_id));
    pos_out.write((const char*)&method_id, sizeof(method_id));
    pos_out.write((const char*)&file_id, sizeof(file_id));
    pos_out.write((const char*)&pos->line, sizeof(pos->line));
    pos_out.write((const char*)&parent_line, sizeof(parent_line));
  }

  std::ofstream ofs(m_filename_v2.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  uint32_t magic = 0xfaceb000; // serves as endianess check
  ofs.write((const char*)&magic, sizeof(magic));
  uint32_t version = 2;
  ofs.write((const char*)&version, sizeof(version));
  uint32_t spool_count = string_pool.size();
  ofs.write((const char*)&spool_count, sizeof(spool_count));
  for (const auto& s : string_pool) {
    uint32_t ssize = s.size();
    ofs.write((const char*)&ssize, sizeof(ssize));
    ofs << s;
  }
  uint32_t pos_count = m_positions.size();
  ofs.write((const char*)&pos_count, sizeof(pos_count));
  ofs << pos_out.str();
}

PositionMapper* PositionMapper::make(const std::string& map_filename_v2) {
  if (map_filename_v2.empty()) {
    // If no path is provided for the map, just pass the original line numbers
    // through to the output. This does mean that the line numbers will be
    // incorrect for inlined code.
    return new NoopPositionMapper();
  } else {
    return new RealPositionMapper(map_filename_v2);
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
