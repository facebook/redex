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

  bool is_initialized() const { return m_initialized; }

  bool has_stats() const { return !m_method_stats.empty(); }

  // Get the method profiles for some interaction. If no argument is given, the
  // interactions are searched first by the default interaction_id (empty
  // string), then by the interaction named "ColdStart".
  //
  // If no interactions are found. Return an empty map.
  //
  // TODO(T66774154): This default argument strategy is error-prone
  const StatsMap& method_stats(
      boost::optional<std::string> interaction_id = boost::none) const;

  const AllInteractions& all_interactions() const {
    return m_method_stats;
  }

 private:
  AllInteractions m_method_stats;
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

} // namespace method_profiles
