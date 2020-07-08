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

  size_t unresolved_size() const {
    return m_unresolved_lines.size();
  }

  // Get the method profiles for some interaction id.
  // If no interactions are found by that interaction id, Return an empty map.
  const StatsMap& method_stats(const std::string& interaction_id) const;

  const AllInteractions& all_interactions() const {
    return m_method_stats;
  }

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

inline unsigned int get_method_weight_if_available(
    const DexMethod* method,
    const StatsMap& stats_map) {
  auto it = stats_map.find(method);
  if (it != stats_map.end() && it->second.appear_percent >= 95) {
    return 100;
  }

  // If the method is not present in profiled order file we'll put it in the
  // end of the code section
  return 0;
}

inline unsigned int get_method_weight_override(
    const DexMethod* method,
    const std::unordered_set<std::string>* whitelisted_substrings) {
  const std::string& deobfname = method->get_deobfuscated_name();
  for (const std::string& substr : *whitelisted_substrings) {
    if (deobfname.find(substr) != std::string::npos) {
      return 100;
    }
  }

  return 0;
}

struct dexmethods_profiled_comparator {
  const StatsMap& stats_map;
  const std::unordered_set<std::string>* whitelisted_substrings;
  // This cache should be a pointer so that copied
  // comparators can still share the same cache for better performance
  std::unordered_map<DexMethod*, unsigned int>* cache;

  dexmethods_profiled_comparator(
      const method_profiles::MethodProfiles& method_profiles,
      const std::unordered_set<std::string>* whitelisted_substrings_val,
      std::unordered_map<DexMethod*, unsigned int>* cache)
      : stats_map(method_profiles.method_stats(COLD_START)),
        whitelisted_substrings(whitelisted_substrings_val),
        cache(cache) {
    always_assert(whitelisted_substrings_val != nullptr);
    always_assert(cache != nullptr);
  }

  bool operator()(DexMethod* a, DexMethod* b) {
    if (a == nullptr) {
      return b != nullptr;
    } else if (b == nullptr) {
      return false;
    }

    auto get_weight = [this](DexMethod* m) -> unsigned int {
      const auto& search = cache->find(m);
      if (search != cache->end()) {
        return search->second;
      }

      auto w = get_method_weight_if_available(m, stats_map);
      if (w == 0) {
        // For methods not included in the profiled methods file, move them to
        // the top section anyway if they match one of the whitelisted
        // substrings.
        w = get_method_weight_override(m, whitelisted_substrings);
      }

      cache->emplace(m, w);
      return w;
    };

    unsigned int weight_a = get_weight(a);
    unsigned int weight_b = get_weight(b);

    if (weight_a == weight_b) {
      return compare_dexmethods(a, b);
    }

    return weight_a > weight_b;
  }
};

} // namespace method_profiles
