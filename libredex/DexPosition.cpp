/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <sstream>

#include "DexClass.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "RedexContext.h"
#include "Show.h"
#include "Trace.h"

DexPosition::DexPosition(uint32_t line) : line(line) {}

DexPosition::DexPosition(const DexString* method,
                         const DexString* file,
                         uint32_t line)
    : method(method), file(file), line(line) {}

void DexPosition::bind(const DexString* method_, const DexString* file_) {
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
  const DexString* source = nullptr;
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

PositionPatternSwitchManager::PositionPatternSwitchManager()
    : m_pattern_string(DexString::make_string("Lredex/$Position;.pattern:()V")),
      m_switch_string(DexString::make_string("Lredex/$Position;.switch:()V")),
      m_unknown_source_string(DexString::make_string("UnknownSource")) {}

DexPosition* PositionPatternSwitchManager::internalize(DexPosition* pos) {
  always_assert(pos);
  auto it = m_positions.find(pos);
  if (it != m_positions.end()) {
    return it->second.get();
  }

  auto cloned_position = new DexPosition(*pos);
  if (pos->parent) {
    cloned_position->parent = internalize(pos->parent);
  }
  m_positions.emplace(cloned_position,
                      std::unique_ptr<DexPosition>(cloned_position));
  return cloned_position;
}

uint32_t PositionPatternSwitchManager::make_pattern(
    PositionPattern pos_pattern) {
  for (auto& pos : pos_pattern) {
    always_assert(pos->file);
    pos = internalize(pos);
  }
  auto it = m_patterns_map.find(pos_pattern);
  if (it == m_patterns_map.end()) {
    it = m_patterns_map.emplace(pos_pattern, m_patterns.size()).first;
    m_patterns.push_back(std::move(pos_pattern));
  }
  return it->second;
}

uint32_t PositionPatternSwitchManager::make_switch(PositionSwitch pos_switch) {
  for (auto& c : pos_switch) {
    always_assert(c.position);
    always_assert(c.position->file);
    c.position = internalize(c.position);
  }
  auto it = m_switches_map.find(pos_switch);
  if (it == m_switches_map.end()) {
    it = m_switches_map.emplace(pos_switch, m_switches.size()).first;
    m_switches.push_back(std::move(pos_switch));
  }
  return it->second;
}

std::unique_ptr<DexPosition>
PositionPatternSwitchManager::make_pattern_position(uint32_t pattern_id) const {
  always_assert(pattern_id < m_patterns.size());
  return std::make_unique<DexPosition>(m_pattern_string,
                                       m_unknown_source_string, pattern_id);
}

std::unique_ptr<DexPosition> PositionPatternSwitchManager::make_switch_position(
    uint32_t switch_id) const {
  always_assert(switch_id < m_switches.size());
  return std::make_unique<DexPosition>(m_switch_string, m_unknown_source_string,
                                       switch_id);
}

void RealPositionMapper::register_position(DexPosition* pos) {
  always_assert(pos->file);
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

void RealPositionMapper::process_pattern_switch_positions() {
  auto manager = g_redex->get_position_pattern_switch_manager();
  if (manager->empty()) {
    return;
  }

  // First. we find all reachable patterns, switches and cases.
  auto switches = manager->get_switches();
  std::unordered_set<uint32_t> reachable_patterns;
  std::unordered_set<uint32_t> reachable_switches;
  std::unordered_set<DexPosition*> visited;
  std::unordered_map<uint32_t, std::vector<PositionCase>> pending;
  std::function<void(DexPosition*)> visit;
  visit = [&](DexPosition* pos) {
    always_assert(pos);
    for (; pos && visited.insert(pos).second; pos = pos->parent) {
      if (manager->is_pattern_position(pos)) {
        if (reachable_patterns.insert(pos->line).second) {
          auto it = pending.find(pos->line);
          if (it != pending.end()) {
            for (auto c : it->second) {
              visit(c.position);
            }
            pending.erase(pos->line);
          }
        }
      } else if (manager->is_switch_position(pos)) {
        if (reachable_switches.insert(pos->line).second) {
          for (auto& c : switches.at(pos->line)) {
            if (reachable_patterns.count(c.pattern_id)) {
              visit(c.position);
            } else {
              pending[c.pattern_id].push_back(c);
            }
          }
        }
      }
    }
  };
  for (auto pos : m_positions) {
    visit(pos);
  }

  auto count_string = DexString::make_string("Lredex/$Position;.count:()V");
  auto case_string = DexString::make_string("Lredex/$Position;.case:()V");
  auto unknown_source_string = DexString::make_string("UnknownSource");

  // Second, we encode the switches and cases via extra positions.
  // For example, at some start line, we'll create 3 consecutive entries such
  // as the following, where 12345 and 54321 are pattern-ids.
  //
  // ...
  // 23: (some actual position)
  // ...
  // 42: (some actual position)
  // ...
  // 101: method Lredex/$Position;.count:()V, line 2 (no parent)
  // 102: method Lredex/$Position;.case:()V, line 12345, parent 23
  // 103: method Lredex/$Position;.case:()V, line 54321, parent 42
  std::unordered_map<uint32_t, uint32_t> switch_line_map;
  for (uint32_t switch_id = 0; switch_id < switches.size(); ++switch_id) {
    if (!reachable_switches.count(switch_id)) {
      continue;
    }
    // We go over cases once to make sure all referenced positions are
    // registered and fully initialized. Note that only positions with a valid
    // file are considered.
    std::vector<PositionCase> reachable_cases;
    for (auto& c : switches.at(switch_id)) {
      if (!reachable_patterns.count(c.pattern_id)) {
        continue;
      }
      for (auto pos = c.position; pos && pos->file; pos = pos->parent) {
        auto it = m_pos_line_map.find(pos);
        if (it != m_pos_line_map.end()) {
          always_assert(it->second != -1);
          break;
        }
        auto idx = m_positions.size();
        m_positions.emplace_back(pos);
        m_pos_line_map.emplace(pos, idx);
      }
      reachable_cases.push_back(c);
    }
    // Sort cases by pattern-id, so that we can later do a binary search when
    // finding a matching pattern-id
    std::sort(reachable_cases.begin(), reachable_cases.end(),
              [](const PositionCase& a, const PositionCase& b) {
                return a.pattern_id < b.pattern_id;
              });
    // We emit a first entry holding the count
    switch_line_map.emplace(switch_id, m_positions.size());
    {
      auto count_pos = new DexPosition(count_string, unknown_source_string,
                                       reachable_cases.size());
      m_owned_auxiliary_positions.emplace_back(count_pos);
      auto idx = m_positions.size();
      m_positions.emplace_back(count_pos);
      m_pos_line_map[count_pos] = idx;
    }
    // Then we emit consecutive list of cases
    for (auto& c : reachable_cases) {
      auto case_pos =
          new DexPosition(case_string, unknown_source_string, c.pattern_id);
      m_owned_auxiliary_positions.emplace_back(case_pos);
      always_assert(c.position);
      always_assert(c.position->file);
      case_pos->parent = c.position;
      auto idx = m_positions.size();
      m_positions.emplace_back(case_pos);
      m_pos_line_map[case_pos] = idx;
    }
  }

  // Finally, we rewrite all switch positions to reference the emitted case
  // lists. For the above example, if the case-list for some switch_id was
  // emitted starting at line 101, then we'll update the referencing position to
  //
  // (some line): method Lredex/$Position;.switch:()V, line 101
  //
  // Note that the callsite remain unchanged, still referencing the pattern-id,
  // e.g.
  //
  // (some line): method Lredex/$Position;.pattern:()V, line 12345
  //
  // TODO: Should we undo this when we are done writing the map?
  for (auto pos : m_positions) {
    if (manager->is_switch_position(pos)) {
      pos->line = switch_line_map.at(pos->line);
    }
  }
}

uint32_t RealPositionMapper::size() const { return m_positions.size(); }

void RealPositionMapper::write_map_v2() {
  // to ensure that the line numbers in the Dex are as compact as possible,
  // we put the emitted positions at the start of the list and rest at the end
  for (auto& p : m_pos_line_map) {
    auto pos = p.first;
    auto& line = p.second;
    if (line == -1) {
      auto idx = m_positions.size();
      m_positions.emplace_back(pos);
      line = idx;
    }
  }

  process_pattern_switch_positions();

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
  std::unordered_map<std::string_view, uint32_t> string_ids;
  std::vector<std::unique_ptr<std::string>> string_pool;

  auto id_of_string = [&](const std::string_view s) -> uint32_t {
    auto it = string_ids.find(s);
    if (it == string_ids.end()) {
      auto p = std::make_unique<std::string>(s);
      it = string_ids.emplace(*p, string_pool.size()).first;
      string_pool.push_back(std::move(p));
    }
    return it->second;
  };

  size_t unregistered_parent_positions{0};

  for (auto pos : m_positions) {
    uint32_t parent_line = 0;
    try {
      parent_line = pos->parent == nullptr ? 0 : get_line(pos->parent);
    } catch (std::out_of_range& e) {
      ++unregistered_parent_positions;
      TRACE(OPUT, 1, "Parent position %s of %s was not registered",
            SHOW(pos->parent), SHOW(pos));
    }
    // of the form "class_name.method_name:(arg_types)return_type"
    const auto full_method_name = pos->method->str();
    // strip out the args and return type
    const auto qualified_method_name =
        full_method_name.substr(0, full_method_name.find(':'));
    auto class_name = java_names::internal_to_external(
        qualified_method_name.substr(0, qualified_method_name.rfind('.')));
    auto method_name =
        qualified_method_name.substr(qualified_method_name.rfind('.') + 1);
    auto class_id = id_of_string(class_name);
    auto method_id = id_of_string(method_name);
    auto file_id = id_of_string(pos->file->str());
    pos_out.write((const char*)&class_id, sizeof(class_id));
    pos_out.write((const char*)&method_id, sizeof(method_id));
    pos_out.write((const char*)&file_id, sizeof(file_id));
    pos_out.write((const char*)&pos->line, sizeof(pos->line));
    pos_out.write((const char*)&parent_line, sizeof(parent_line));
  }

  if (unregistered_parent_positions > 0 && !traceEnabled(OPUT, 1)) {
    TRACE(OPUT, 0,
          "%zu parent positions had not been registered. Run with TRACE=OPUT:1 "
          "to list them.",
          unregistered_parent_positions);
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
    uint32_t ssize = s->size();
    ofs.write((const char*)&ssize, sizeof(ssize));
    ofs << *s;
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

const DexString* RealPositionMapper::get_source_file(const DexClass*) {
  // Note: When remapping line numbers, we don't simply emit DEX_NO_INDEX for
  // the source_file_idx because that would cause stack traces to print
  // "at com.foo.bar (Unknown source)" even when line number data is
  // available. So we make the source_file_idx point at an empty string
  // instead.
  return DexString::make_string("");
}

const DexString* NoopPositionMapper::get_source_file(const DexClass* clz) {
  return clz->get_source_file();
}
