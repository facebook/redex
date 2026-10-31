/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "ConcurrentContainers.h"
#include "DeterministicContainers.h"

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
// Identity of an interned position. Keep these two adjacent: they are a pair,
// and a hash that stops agreeing with its equality is the bug this replaced.
// A parent is interned before its child, so a whole inlining chain is
// identified by one pointer and neither of these recurses.
struct InternedPositionHash {
  size_t operator()(const DexPosition& pos) const {
    size_t h = reinterpret_cast<size_t>(pos.method);
    boost::hash_combine(h, reinterpret_cast<size_t>(pos.file));
    boost::hash_combine(h, pos.line);
    boost::hash_combine(h, reinterpret_cast<size_t>(pos.parent));
    return h;
  }
};
struct InternedPositionEqual {
  bool operator()(const DexPosition& a, const DexPosition& b) const {
    return a.method == b.method && a.file == b.file && a.line == b.line &&
           a.parent == b.parent;
  }
};

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
  return c.pattern_id ^ reinterpret_cast<size_t>(c.position);
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

  // TODO: Enable the following flag. It's off for now to ensure that the
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

  // How many positions `internalize` was asked for, and how many distinct ones
  // it kept. The gap between them is the clone the interning avoided; when the
  // interning was broken the two were equal.
  size_t get_num_internalize_calls() const {
    return m_internalize_calls.load();
  }
  size_t get_num_interned_positions() const { return m_positions.size(); }

 private:
  DexPosition* internalize(DexPosition* pos);

  // The element IS the manager's copy of the position, so no key can outlive
  // what it names. Nothing is ever erased and elements are pinned for the
  // container's lifetime, so a pointer to one stays valid for the run. Do not
  // mutate an interned position: it is its own key -- which is also why
  // provenance is kept out of the key, so `mark_unreliable` stays safe.
  //
  // Concurrent because the manager hangs off RedexContext and any pass can
  // reach it, and making canonicalisation thread-safe costs one atomic. The id
  // tables below are a separate matter: they are unsynchronised, so a caller
  // that registers from more than one thread has to serialise them itself.
  InsertOnlyConcurrentSet<DexPosition,
                          InternedPositionHash,
                          InternedPositionEqual>
      m_positions;
  std::atomic<size_t> m_internalize_calls{0};

  UnorderedMap<PositionPattern, uint32_t, PositionPatternHasher> m_patterns_map;
  std::vector<PositionPattern> m_patterns;
  UnorderedMap<PositionSwitch, uint32_t, PositionSwitchHasher> m_switches_map;
  std::vector<PositionSwitch> m_switches;

  const DexString* m_pattern_string;
  const DexString* m_switch_string;
  const DexString* m_unknown_source_string;
};

class PositionMapper {
 public:
  virtual ~PositionMapper() {}
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
  UnorderedMap<DexPosition*, int64_t> m_pos_line_map;
  std::queue<DexPosition*> m_possibly_incomplete_positions;
  std::vector<std::unique_ptr<DexPosition>> m_owned_auxiliary_positions;

  void process_pattern_switch_positions();

 protected:
  int64_t add_position(DexPosition* pos);
  uint32_t get_line(DexPosition*);
  void write_map_v2();

 public:
  explicit RealPositionMapper(std::string filename_v2)
      : m_filename_v2(std::move(filename_v2)) {}
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
