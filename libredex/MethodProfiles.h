/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace method_profiles {

// These names (and their order) match the columns of the csv
enum {
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

  const std::unordered_map<const DexMethodRef*, Stats>& method_stats() const {
    return m_method_stats;
  }

 private:
  std::unordered_map<const DexMethodRef*, Stats> m_method_stats;
  bool m_initialized{false};
  // Read a "simple" csv file (no quoted commas or extra spaces) and populate
  // m_method_stats
  bool parse_stats_file(const std::string& csv_filename);
  // Read a line fromt the "simple" csv file and put an entry into
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
