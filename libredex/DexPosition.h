/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class DexClass;
class DexMethod;
class DexString;
class DexDebugItem;

struct DexPosition final {
  const DexString* method{nullptr};
  const DexString* file{nullptr};
  uint32_t line;
  // when a function gets inlined for the first time, all its DexPositions will
  // have the DexPosition of the callsite as their parent.
  DexPosition* parent{nullptr};
  explicit DexPosition(const DexString* file, uint32_t line);
  DexPosition(const DexString* method, const DexString* file, uint32_t line);

  void bind(const DexString* method_, const DexString* file_);
  void bind(const DexString* method_);
  bool operator==(const DexPosition&) const;

  static std::unique_ptr<DexPosition> make_synthetic_entry_position(
      const DexMethod* method);
};
inline size_t hash_value(const DexPosition* pos) {
  return pos == nullptr ? 0
                        : ((size_t)pos->method + (size_t)pos->file + pos->line +
                           hash_value(pos->parent));
}
using ConstDexPositionPtrHasher = boost::hash<const DexPosition*>;

inline size_t hash_value(const DexPosition& pos) {
  return (size_t)pos.method + (size_t)pos.file + pos.line +
         (pos.parent == nullptr ? 0 : hash_value(*pos.parent));
}
using DexPositionHasher = boost::hash<DexPosition>;

using PositionPattern = std::vector<DexPosition*>;
using PositionPatternHasher = boost::hash<PositionPattern>;

struct PositionCase {
  uint32_t pattern_id;
  DexPosition* position;
  bool operator==(const PositionCase& other) const {
    return pattern_id == other.pattern_id && position == other.position;
  }
};
inline size_t hash_value(const PositionCase& c) {
  return c.pattern_id ^ (size_t)c.position;
}
using PositionCaseHasher = boost::hash<PositionCase>;

using PositionSwitch = std::vector<PositionCase>;
using PositionSwitchHasher = boost::hash<PositionSwitch>;

/*
 * This manager class maintains state representing patterns and position
 * switches that can be used when outlining. It can be accessed via the
 * RedexContext.
 */
class PositionPatternSwitchManager {
 public:
  PositionPatternSwitchManager();

  // TODO: Enable the following flag. It's off for now to ensures that the
  // inliner and outliner never produce an outlined method that invokes an
  // outlined method, a limitation imposed by symbolication infrastructure.
  static constexpr bool CAN_OUTLINED_METHOD_INVOKE_OUTLINED_METHOD = false;

  // Returns a value that uniquely identifies the pattern.
  uint32_t make_pattern(PositionPattern pos_pattern);

  // Returns a value that uniquely identifies the switch.
  uint32_t make_switch(PositionSwitch pos_switch);

  // A position that can be used at an outlined method call-site to indicate
  // that a particular position pattern in the outlined method should be
  // selected. For example, for pattern-id 12345, this will produce the
  // following position:
  //
  // method Lredex/$Position;.pattern:()V, line 12345 (no parent)
  std::unique_ptr<DexPosition> make_pattern_position(uint32_t pattern_id) const;

  // A position that can be used in an outlined method to indicate a choice
  // between different positions, dependent on a particular call-site pattern.
  // For example, for switch-id 99991, this will produce the following position:
  //
  // method Lredex/$Position;.switch:()V, line 99991 (no parent)
  //
  // Note that later, when a v2-map file is created, the line number will be
  // replaced with an offset to a switch-case table, so the switch-ids used
  // while Redex is running won't be found in the map file.
  std::unique_ptr<DexPosition> make_switch_position(uint32_t switch_id) const;

  bool is_pattern_position(DexPosition* pos) const {
    return pos->method == m_pattern_string;
  }
  bool is_switch_position(DexPosition* pos) const {
    return pos->method == m_switch_string;
  }

  bool empty() const {
    return m_positions.empty() && m_patterns.empty() && m_switches.empty();
  }

  const std::vector<PositionPattern>& get_patterns() const {
    return m_patterns;
  }

  const std::vector<PositionSwitch>& get_switches() const { return m_switches; }

 private:
  DexPosition* internalize(DexPosition* pos);

  std::unordered_map<const DexPosition*,
                     std::unique_ptr<DexPosition>,
                     ConstDexPositionPtrHasher>
      m_positions;
  std::unordered_map<PositionPattern, uint32_t, PositionPatternHasher>
      m_patterns_map;
  std::vector<PositionPattern> m_patterns;
  std::unordered_map<PositionSwitch, uint32_t, PositionSwitchHasher>
      m_switches_map;
  std::vector<PositionSwitch> m_switches;

  const DexString* m_pattern_string;
  const DexString* m_switch_string;
  const DexString* m_unknown_source_string;
};

class PositionMapper {
 public:
  virtual ~PositionMapper(){};
  virtual const DexString* get_source_file(const DexClass*) = 0;
  virtual uint32_t position_to_line(DexPosition*) = 0;
  virtual void register_position(DexPosition* pos) = 0;
  virtual void write_map() = 0;
  virtual uint32_t size() const = 0;
  static PositionMapper* make(const std::string& map_filename_v2);
};

/*
 * This allows us to recover the original file names and line numbers from
 * runtime stack traces of Dex files that have undergone inlining. The
 * PositionMapper produces a text file with this data, and the line numbers in
 * the Dex debug info indicate the line in this text file at which the real
 * position can be found.
 */
class RealPositionMapper : public PositionMapper {
  std::string m_filename_v2;
  std::vector<DexPosition*> m_positions;
  std::unordered_map<DexPosition*, int64_t> m_pos_line_map;
  std::queue<DexPosition*> m_possibly_incomplete_positions;
  std::vector<std::unique_ptr<DexPosition>> m_owned_auxiliary_positions;

  void process_pattern_switch_positions();

 protected:
  int64_t add_position(DexPosition* pos);
  uint32_t get_line(DexPosition*);
  void write_map_v2();

 public:
  explicit RealPositionMapper(const std::string& filename_v2)
      : m_filename_v2(filename_v2) {}
  const DexString* get_source_file(const DexClass*) override;
  uint32_t position_to_line(DexPosition*) override;
  void register_position(DexPosition* pos) override;
  void write_map() override;
  uint32_t size() const override;
};

class NoopPositionMapper : public PositionMapper {
 public:
  const DexString* get_source_file(const DexClass*) override;
  uint32_t position_to_line(DexPosition* pos) override { return pos->line; }
  void register_position(DexPosition* pos) override {}
  void write_map() override {}
  uint32_t size() const override { return 0; }
};
