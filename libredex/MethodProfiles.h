/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"
#include "Timer.h"
#include "Trace.h"

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

enum ParsingMode {
  // NONE is the initial state. We haven't parsed a header yet
  NONE,
  MAIN,
  METADATA,
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
  int16_t min_api_level{0}; // min_api_level
};

using StatsMap = std::unordered_map<const DexMethodRef*, Stats>;
using AllInteractions = std::map<std::string, StatsMap>;
const std::string COLD_START = "ColdStart";

class MethodProfiles {
 public:
  MethodProfiles() {}

  void initialize(const std::vector<std::string>& csv_filenames) {
    m_initialized = true;
    Timer t("Parsing agg_method_stats_files");
    for (const std::string& csv_filename : csv_filenames) {
      m_interaction_id = "";
      m_mode = NONE;
      bool success = parse_stats_file(csv_filename);
      if (!success) {
        TRACE(METH_PROF,
              1,
              "WARNING: parsing of %s failed",
              csv_filename.c_str());
      }
      // FIXME re-enable this check
      // always_assert_log(success,
      //                   "Failed to parse %s. See stderr for more details",
      //                   csv_filename.c_str());
    }
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
    size_t result = 0;
    for (const auto& pair : m_unresolved_lines) {
      result += pair.second.size();
    }
    return result;
  }

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

  boost::optional<uint32_t> get_interaction_count(
      const std::string& interaction_id) const;

  // Try to resolve previously unresolved lines
  void process_unresolved_lines();

 private:
  AllInteractions m_method_stats;
  // Resolution may fail because of renaming or generated methods. Store the
  // unresolved lines here (per interaction) so we can update after passes run
  // and change the names of methods
  std::unordered_map<std::string, std::vector<std::string>> m_unresolved_lines;
  ParsingMode m_mode{NONE};
  // A map from interaction ID to the number of times that interaction was
  // triggered. This can be used to compare relative prevalence of different
  // interactions.
  std::unordered_map<std::string, uint32_t> m_interaction_counts;
  // A map from column index to column header
  std::unordered_map<uint32_t, std::string> m_optional_columns;
  // The interaction id from the metadata at the top of the file
  std::string m_interaction_id;
  bool m_initialized{false};

  // Read a "simple" csv file (no quoted commas or extra spaces) and populate
  // m_method_stats
  bool parse_stats_file(const std::string& csv_filename);

  // Read a line of data (not a header)
  bool parse_line(std::string& line);
  // Read a line from the main section of the aggregated stats file and put an
  // entry into m_method_stats
  bool parse_main(std::string& line);
  // Read a line of data from the metadata section (at the top of the file)
  bool parse_metadata(std::string& line);

  // Parse the first line and make sure it matches our expectations
  bool parse_header(std::string& line);
};

// NOTE: Do not use this comparator directly in `std::sort` calls, as it is
// stateful. libstdc++ copies the comparator during sorting. Instead, use
// `std::ref` of a local instance.
class dexmethods_profiled_comparator {
  const MethodProfiles* m_method_profiles;
  const std::unordered_set<std::string>* m_allowlisted_substrings;
  std::unordered_map<DexMethod*, double> m_cache;
  bool m_legacy_order;
  std::vector<std::string> m_interactions;

  const DexMethod* m_coldstart_start_marker;
  const DexMethod* m_coldstart_end_marker;

  std::unordered_map<DexMethod*, size_t> m_initial_order;

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
      const std::vector<DexMethod*>& initial_order,
      const method_profiles::MethodProfiles* method_profiles,
      const std::unordered_set<std::string>* allowlisted_substrings,
      bool legacy_order);

  // See class comment.
  dexmethods_profiled_comparator(const dexmethods_profiled_comparator&) =
      delete;

  bool operator()(DexMethod* a, DexMethod* b);
};

} // namespace method_profiles
