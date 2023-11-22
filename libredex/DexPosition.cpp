/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <sstream>
#include <type_traits>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "RedexContext.h"
#include "Show.h"
#include "Trace.h"
#include "WorkQueue.h"

DexPosition::DexPosition(const DexString* file, uint32_t line)
    : file(file), line(line) {
  always_assert(file != nullptr);
}

DexPosition::DexPosition(const DexString* method,
                         const DexString* file,
                         uint32_t line)
    : method(method), file(file), line(line) {
  always_assert(file != nullptr);
}

void DexPosition::bind(const DexString* method_, const DexString* file_) {
  always_assert(file_ != nullptr);
  this->method = method_;
  this->file = file_;
}

void DexPosition::bind(const DexString* method_) { this->method = method_; }

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
  auto [_, emplaced] = m_pos_line_map.emplace(pos, -1);
  if (emplaced) {
    m_possibly_incomplete_positions.push(pos);
  }
}

int64_t RealPositionMapper::add_position(DexPosition* pos) {
  auto [it, _] = m_pos_line_map.emplace(pos, -1);
  if (it->second == -1) {
    it->second = m_positions.size();
    m_positions.push_back(pos);
  }
  return it->second;
}

uint32_t RealPositionMapper::get_line(DexPosition* pos) {
  return m_pos_line_map.at(pos) + 1;
}

uint32_t RealPositionMapper::position_to_line(DexPosition* pos) {
  return add_position(pos) + 1;
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
  ConcurrentMap<uint32_t, std::vector<PositionCase>> reachable_patterns;
  InsertOnlyConcurrentSet<uint32_t> reachable_switches;
  InsertOnlyConcurrentSet<DexPosition*> visited;
  std::function<void(DexPosition*)> visit;
  visit = [&](DexPosition* pos) {
    always_assert(pos);
    for (; pos && visited.insert(pos).second; pos = pos->parent) {
      if (manager->is_pattern_position(pos)) {
        std::vector<PositionCase> cases;
        reachable_patterns.update(pos->line,
                                  [&](auto, auto& pending, bool exists) {
                                    if (exists) {
                                      cases = std::move(pending);
                                    }
                                  });
        for (auto& c : cases) {
          visit(c.position);
        }
      } else if (manager->is_switch_position(pos)) {
        if (reachable_switches.insert(pos->line).second) {
          for (auto& c : switches.at(pos->line)) {
            bool pattern_reachable = false;
            reachable_patterns.update(c.pattern_id,
                                      [&](auto, auto& pending, bool exists) {
                                        if (exists && pending.empty()) {
                                          pattern_reachable = true;
                                          return;
                                        }
                                        pending.push_back(c);
                                      });
            if (pattern_reachable) {
              visit(c.position);
            }
          }
        }
      }
    }
  };

  workqueue_run<DexPosition*>(visit, m_positions);

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
        auto [it, emplaced] = m_pos_line_map.emplace(pos, m_positions.size());
        if (emplaced) {
          m_positions.push_back(pos);
        } else {
          always_assert(it->second != -1);
        }
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
      add_position(count_pos);
    }
    // Then we emit consecutive list of cases
    for (auto& c : reachable_cases) {
      auto case_pos =
          new DexPosition(case_string, unknown_source_string, c.pattern_id);
      m_owned_auxiliary_positions.emplace_back(case_pos);
      always_assert(c.position);
      always_assert(c.position->file);
      case_pos->parent = c.position;
      add_position(case_pos);
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
  while (!m_possibly_incomplete_positions.empty()) {
    auto* pos = m_possibly_incomplete_positions.front();
    m_possibly_incomplete_positions.pop();
    auto& line = m_pos_line_map[pos];
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
  InsertOnlyConcurrentMap<std::string_view, uint32_t> string_ids;
  std::array<std::mutex, cc_impl::kDefaultSlots> string_ids_mutex;
  InsertOnlyConcurrentMap<size_t, std::unique_ptr<std::string>> string_pool;
  std::atomic<size_t> next_string_id{0};

  auto id_of_string = [&](std::string_view s) -> uint32_t {
    const uint32_t* opt_id = string_ids.get(s);
    if (opt_id) {
      return *opt_id;
    }
    auto p = std::make_unique<std::string>(s);
    size_t bucket = std::hash<std::string_view>{}(s) % string_ids_mutex.size();
    size_t id;
    {
      std::lock_guard<std::mutex> lock(string_ids_mutex[bucket]);
      opt_id = string_ids.get(s);
      if (opt_id) {
        return *opt_id;
      }
      id = next_string_id.fetch_add(1);
      string_ids.emplace(*p, id);
    }
    string_pool.emplace(id, std::move(p));
    return id;
  };

  std::atomic<size_t> unregistered_parent_positions{0};

  static_assert(std::is_same<decltype(m_positions[0]->line), uint32_t>::value);
  static_assert(std::is_same<decltype(id_of_string("")), uint32_t>::value);
  std::vector<uint32_t> pos_data;
  pos_data.resize(5 * m_positions.size());

  workqueue_run_for<size_t>(0, m_positions.size(), [&](size_t idx) {
    auto* pos = m_positions[idx];
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
    pos_data[5 * idx + 0] = class_id;
    pos_data[5 * idx + 1] = method_id;
    pos_data[5 * idx + 2] = file_id;
    pos_data[5 * idx + 3] = pos->line;
    pos_data[5 * idx + 4] = parent_line;
  });
  always_assert(pos_data.size() == 5 * m_positions.size());

  if (unregistered_parent_positions.load() > 0 && !traceEnabled(OPUT, 1)) {
    TRACE(OPUT, 0,
          "%zu parent positions had not been registered. Run with TRACE=OPUT:1 "
          "to list them.",
          unregistered_parent_positions.load());
  }

  std::ofstream ofs(m_filename_v2.c_str(),
                    std::ofstream::out | std::ofstream::trunc);
  uint32_t magic = 0xfaceb000; // serves as endianess check
  ofs.write((const char*)&magic, sizeof(magic));
  uint32_t version = 2;
  ofs.write((const char*)&version, sizeof(version));
  uint32_t spool_count = string_pool.size();
  ofs.write((const char*)&spool_count, sizeof(spool_count));
  always_assert(string_pool.size() < std::numeric_limits<uint32_t>::max());
  auto map = std::make_unique<uint32_t[]>(string_pool.size());
  const uint32_t unmapped = 0;
  const uint32_t first_mapped = 1;
  uint32_t next_mapped = first_mapped;
  auto order = [&](uint32_t& string_id) {
    auto& mapped = map[string_id];
    if (mapped == unmapped) {
      const auto& s = string_pool.at(string_id);
      uint32_t ssize = s->size();
      ofs.write((const char*)&ssize, sizeof(ssize));
      ofs << *s;
      mapped = next_mapped++;
    }
    string_id = mapped - first_mapped;
  };
  for (size_t idx = 0; idx < m_positions.size(); ++idx) {
    order(pos_data[5 * idx + 0]); // class_id
    order(pos_data[5 * idx + 1]); // method_id
    order(pos_data[5 * idx + 2]); // file_id
  }
  always_assert(next_mapped - first_mapped == string_pool.size());
  uint32_t pos_count = m_positions.size();
  ofs.write((const char*)&pos_count, sizeof(pos_count));
  ofs.write((const char*)pos_data.data(), sizeof(uint32_t) * pos_data.size());
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
