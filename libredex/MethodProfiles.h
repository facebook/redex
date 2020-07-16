/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

namespace method_profiles {

// These names (and their order) match the columns of the csv
enum : uint8_t {
  INDEX,
  NAME,
  APPEAR100,
  APPEAR_NUMBER,
  AVG_CALL,
  AVG_ORDER,
  AVG_RANK100,
  MIN_API_LEVEL,
};

struct Stats {
  // The percentage of samples that this method appeared in
  double appear_percent{0.0}; // appear100

  // Number of invocations of this method (averaged over all samples)
  double call_count{0.0}; // avg_call

  // Relative index when this method is first executed (averaged over all
  // samples)
  // * 0.0 means the beginning of the measured period
  // * 100.0 means the end of the measured period
  double order_percent{0.0}; // avg_rank100

  // The minimum API level that this method was observed running on
  uint8_t min_api_level{0}; // min_api_level
};

using StatsMap = std::unordered_map<const DexMethodRef*, Stats>;
using AllInteractions = std::map<std::string, StatsMap>;
const std::string COLD_START = "ColdStart";

class MethodProfiles {
 public:
  MethodProfiles() {}

  bool initialize(const std::string& csv_filename) {
    m_initialized = true;
    bool success = parse_stats_file(csv_filename);
    if (!success) {
      m_method_stats.clear();
    }
    return success;
  }

  // For testing purposes.
  static MethodProfiles initialize(
      const std::string& interaction_id,
      std::unordered_map<const DexMethodRef*, Stats> data) {
    MethodProfiles ret{};
    ret.m_initialized = true;
    ret.m_method_stats[interaction_id] = std::move(data);
    return ret;
  }

  bool is_initialized() const { return m_initialized; }

  bool has_stats() const { return !m_method_stats.empty(); }

  size_t size() const {
    size_t sum{0};
    for (auto& p : m_method_stats) {
      sum += p.second.size();
    }
    return sum;
  }

  size_t unresolved_size() const { return m_unresolved_lines.size(); }

  // Get the method profiles for some interaction id.
  // If no interactions are found by that interaction id, Return an empty map.
  const StatsMap& method_stats(const std::string& interaction_id) const;

  const AllInteractions& all_interactions() const { return m_method_stats; }

  boost::optional<Stats> get_method_stat(const std::string& interaction_id,
                                         const DexMethodRef* m) const {
    const auto& stats = method_stats(interaction_id);
    auto it = stats.find(m);
    if (it == stats.end()) {
      return boost::none;
    }
    return it->second;
  }

  // Try to resolve previously unresolved lines
  void process_unresolved_lines();

 private:
  AllInteractions m_method_stats;
  std::vector<std::string> m_unresolved_lines;
  bool m_initialized{false};
  // A map from column index to column header
  std::unordered_map<uint32_t, std::string> m_optional_columns;

  // Read a "simple" csv file (no quoted commas or extra spaces) and populate
  // m_method_stats
  bool parse_stats_file(const std::string& csv_filename);
  // Read a line from the "simple" csv file and put an entry into
  // m_method_stats
  bool parse_line(char* line, bool first);
  // Parse the first line and make sure it matches our expectations
  bool parse_header(char* line);

  template <class Func>
  bool parse_cells(char* line, const Func& parse_cell) {
    char* save_ptr = nullptr;
    char* tok = line;
    uint32_t i = 0;
    // Assuming there are no quoted strings containing commas!
    while ((tok = strtok_r(tok, ",", &save_ptr)) != nullptr) {
      bool success = parse_cell(tok, i);
      if (!success) {
        return false;
      }
      tok = nullptr;
      ++i;
    }
    return true;
  }
};

class dexmethods_profiled_comparator {
  const MethodProfiles* m_method_profiles;
  const std::unordered_set<std::string>* m_whitelisted_substrings;
  // This cache should be a pointer so that copied
  // comparators can still share the same cache for better performance
  std::unordered_map<DexMethod*, double>* m_cache;
  bool m_legacy_order;
  std::vector<std::string> m_interactions;

  const DexMethod* m_coldstart_start_marker;
  const DexMethod* m_coldstart_end_marker;

  // The profiled method order is broken into sections, one section for each
  // interaction. Each section has a range of floating point numbers assigned to
  // it (RANGE_SIZE) and the sections are separated by RANGE_STRIDE (which must
  // be larger than RANGE_SIZE). Stride is larger than size so that there is no
  // overlap between regions.
  //
  // Lower sort_num values correspond to occurring earlier in the dex file
  static constexpr double RANGE_SIZE = 1.0;
  static constexpr double RANGE_STRIDE = 2.0;
  static constexpr double COLD_START_RANGE_BEGIN = 0.0;
  static constexpr double VERY_END = std::numeric_limits<double>::max();

  double get_method_sort_num_override(const DexMethod* method);
  double get_method_sort_num(const DexMethod* method);

 public:
  dexmethods_profiled_comparator(
      const method_profiles::MethodProfiles* method_profiles,
      const std::unordered_set<std::string>* whitelisted_substrings,
      std::unordered_map<DexMethod*, double>* cache,
      bool legacy_order);

  bool operator()(DexMethod* a, DexMethod* b);
};

} // namespace method_profiles
