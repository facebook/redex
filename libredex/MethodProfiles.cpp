/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodProfiles.h"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>

#include "CppUtil.h"
#include "GlobalConfig.h"
#include "Show.h"

using namespace method_profiles;

extern int errno;

namespace {

template <class Func>
bool parse_cells(std::string_view line, const Func& parse_cell) {
  char* save_ptr = nullptr;
  uint32_t i = 0;
  // Assuming there are no quoted strings containing commas!
  for (auto cell : split_string(line, ",")) {
    if (cell.back() == '\n') {
      cell.remove_suffix(1);
    }
    bool success = parse_cell(cell, i);
    if (!success) {
      return false;
    }
    ++i;
  }
  return true;
}

bool empty_column(std::string_view sv) { return sv.empty() || sv == "\n"; }

} // namespace

const StatsMap& MethodProfiles::method_stats(
    const std::string& interaction_id) const {
  const auto& search1 = m_method_stats.find(interaction_id);
  if (search1 != m_method_stats.end()) {
    return search1->second;
  }
  if (interaction_id == COLD_START) {
    // Originally, the stats file had no interaction_id column and it only
    // covered coldstart. Search for the default (empty string) for backwards
    // compatibility when we're searching for coldstart but it's not found.
    const auto& search2 = m_method_stats.find("");
    if (search2 != m_method_stats.end()) {
      return search2->second;
    }
  }

  static StatsMap empty_map = {};
  return empty_map;
}

bool MethodProfiles::parse_stats_file(const std::string& csv_filename) {
  TRACE(METH_PROF, 3, "input csv filename: %s", csv_filename.c_str());
  if (csv_filename.empty()) {
    TRACE(METH_PROF, 2, "No csv file given");
    return false;
  }

  // Using C-style file reading and parsing because it's faster than the
  // iostreams equivalent and we expect to read very large csv files.
  std::ifstream ifs(csv_filename);
  if (!ifs.good()) {
    std::cerr << "FAILED to open " << csv_filename << std::endl;
    return false;
  }

  // getline will allocate a buffer and put a pointer to it here
  std::string line;
  while (std::getline(ifs, line)) {
    bool success = false;
    if (m_mode == NONE) {
      success = parse_header(line);
    } else {
      success = parse_line(line);
    }
    if (!success) {
      return false;
    }
  }
  if (ifs.bad()) {
    std::cerr << "FAILED to read a line!" << std::endl;
    return false;
  }

  TRACE(METH_PROF, 1,
        "MethodProfiles successfully parsed %zu rows; %zu unresolved lines",
        size(), unresolved_size());
  return true;
}

// `strtol` and `strtod` requires c string to be null terminated,
// std::string_view::data() doesn't have this guarantee. Our `string_view`s are
// taken from `std::string`s. This should be safe.
template <typename IntType>
IntType parse_int(std::string_view tok) {
  char* ptr = nullptr;
  const auto parsed = strtol(tok.data(), &ptr, 10);
  always_assert_log(
      ptr <= (tok.data() + tok.size()),
      "strtod went over std::string_view boundary for string_view %s",
      SHOW(tok));

  std::string_view rest(ptr, tok.size() - (ptr - tok.data()));
  always_assert_log(ptr != tok.data(), "can't parse %s into a int", SHOW(tok));
  always_assert_log(empty_column(rest), "can't parse %s into a int", SHOW(tok));
  always_assert(parsed <= std::numeric_limits<IntType>::max());
  always_assert(parsed >= std::numeric_limits<IntType>::min());
  return static_cast<IntType>(parsed);
}

double parse_double(std::string_view tok) {
  char* ptr = nullptr;
  const auto result = strtod(tok.data(), &ptr);
  always_assert_log(
      ptr <= (tok.data() + tok.size()),
      "strtod went over std::string_view boundary for string_view %s",
      SHOW(tok));

  std::string_view rest(ptr, tok.size() - (ptr - tok.data()));
  always_assert_log(ptr != tok.data(), "can't parse %s into a double",
                    SHOW(tok));
  always_assert_log(empty_column(rest), "can't parse %s into a double",
                    SHOW(tok));
  return result;
}

bool MethodProfiles::parse_metadata(std::string_view line) {
  always_assert(m_mode == METADATA);
  uint32_t interaction_count{0};
  auto parse_cell = [&](std::string_view cell, uint32_t col) -> bool {
    switch (col) {
    case 0:
      m_interaction_id = std::string(cell);
      return true;
    case 1: {
      interaction_count = parse_int<uint32_t>(cell);
      return true;
    }
    default: {
      bool ok = empty_column(cell);
      if (!ok) {
        std::cerr << "Unexpected extra value in metadata: " << cell
                  << std::endl;
      }
      return ok;
    }
    }
  };
  bool success = parse_cells(line, parse_cell);
  if (!success) {
    return false;
  }
  m_interaction_counts.emplace(m_interaction_id, interaction_count);
  // There should only be one line of metadata per file. Once we've processed
  // it, change the parsing mode back to NONE.
  m_mode = NONE;
  return true;
}

bool MethodProfiles::parse_main(std::string_view line) {
  always_assert(m_mode == MAIN);
  Stats stats;
  std::string interaction_id;
  DexMethodRef* ref = nullptr;
  auto parse_cell = [&](std::string_view cell, uint32_t col) -> bool {
    switch (col) {
    case INDEX:
      // Don't need this raw data. It's an arbitrary index (the line number in
      // the file)
      return true;
    case NAME:
      ref = DexMethod::get_method</*kCheckFormat=*/true>(std::string(cell));
      if (ref == nullptr) {
        TRACE(METH_PROF, 6, "failed to resolve %s", SHOW(cell));
      }
      return true;
    case APPEAR100:
      stats.appear_percent = parse_double(cell);
      return true;
    case APPEAR_NUMBER:
      // Don't need this raw data. appear_percent is the same thing but
      // normalized
      return true;
    case AVG_CALL:
      stats.call_count = parse_double(cell);
      return true;
    case AVG_ORDER:
      // Don't need this raw data. order_percent is the same thing but
      // normalized
      return true;
    case AVG_RANK100:
      stats.order_percent = parse_double(cell);
      return true;
    case MIN_API_LEVEL: {
      stats.min_api_level = parse_int<int16_t>(cell);
      return true;
    }
    default:
      const auto& search = m_optional_columns.find(col);
      if (search != m_optional_columns.end()) {
        if (search->second == "interaction") {
          interaction_id = cell;
          return true;
        }
      }
      std::cerr << "FAILED to parse line. Unknown extra column\n";
      return false;
    }
  };

  bool success = parse_cells(line, parse_cell);
  if (!success) {
    return false;
  }
  if (interaction_id.empty()) {
    // Interaction IDs from the current row have priority over the interaction
    // id from the top of the file. This shouldn't happen in practice, but this
    // is the conservative approach.
    interaction_id = m_interaction_id;
  }
  if (ref != nullptr) {
    TRACE(METH_PROF, 6, "(%s, %s) -> {%f, %f, %f, %d}", SHOW(ref),
          interaction_id.c_str(), stats.appear_percent, stats.call_count,
          stats.order_percent, stats.min_api_level);
    m_method_stats[interaction_id].emplace(ref, stats);
  } else {
    std::string copy(line);
    m_unresolved_lines[interaction_id].push_back(copy);
    TRACE(METH_PROF, 6, "unresolved: %s", copy.c_str());
  }
  return true;
}

bool MethodProfiles::parse_line(std::string_view line) {
  if (m_mode == MAIN) {
    return parse_main(line);
  } else if (m_mode == METADATA) {
    return parse_metadata(line);
  } else {
    always_assert_log(false, "invalid parsing mode");
  }
}

boost::optional<uint32_t> MethodProfiles::get_interaction_count(
    const std::string& interaction_id) const {
  const auto& search = m_interaction_counts.find(interaction_id);
  if (search == m_interaction_counts.end()) {
    return boost::none;
  } else {
    return search->second;
  }
}

void MethodProfiles::process_unresolved_lines() {
  auto unresolved_lines = std::move(m_unresolved_lines);
  m_unresolved_lines.clear();
  for (auto& pair : unresolved_lines) {
    m_interaction_id = pair.first;
    for (auto& line : pair.second) {
      bool success = parse_main(line);
      always_assert(success);
    }
  }

  size_t total_rows = 0;
  for (const auto& pair : m_method_stats) {
    total_rows += pair.second.size();
  }
  TRACE(METH_PROF, 1,
        "After processing unresolved lines: MethodProfiles successfully parsed "
        "%zu rows; %zu unresolved lines",
        total_rows, unresolved_size());
}

bool MethodProfiles::parse_header(std::string_view line) {
  always_assert(m_mode == NONE);
  auto check_cell = [](std::string_view expected, std::string_view cell,
                       uint32_t col) -> bool {
    if (expected != cell) {
      std::cerr << "Unexpected Header (column " << col << "): " << cell
                << " != " << expected << "\n";
      return false;
    }
    return true;
  };
  if (boost::starts_with(line, "interaction")) {
    m_mode = METADATA;
    // Extra metadata at the top of the file that we want to parse
    auto parse_cell = [&](std::string_view cell, uint32_t col) -> bool {
      switch (col) {
      case 0:
        return check_cell("interaction", cell, col);
      case 1:
        return check_cell("appear#", cell, col);
      default: {
        auto ok = empty_column(cell);
        if (!ok) {
          std::cerr << "Unexpected Metadata Column: " << cell << std::endl;
        }
        return ok;
      }
      }
    };
    return parse_cells(line, parse_cell);
  } else {
    m_mode = MAIN;
    auto parse_cell = [&](std::string_view cell, uint32_t col) -> bool {
      switch (col) {
      case INDEX:
        return check_cell("index", cell, col);
      case NAME:
        return check_cell("name", cell, col);
      case APPEAR100:
        return check_cell("appear100", cell, col);
      case APPEAR_NUMBER:
        return check_cell("appear#", cell, col);
      case AVG_CALL:
        return check_cell("avg_call", cell, col);
      case AVG_ORDER:
        return check_cell("avg_order", cell, col);
      case AVG_RANK100:
        return check_cell("avg_rank100", cell, col);
      case MIN_API_LEVEL:
        return check_cell("min_api_level", cell, col);
      default:
        m_optional_columns.emplace(col, std::string(cell));
        return true;
      }
    };
    return parse_cells(line, parse_cell);
  }
}

dexmethods_profiled_comparator::dexmethods_profiled_comparator(
    const std::vector<DexMethod*>& initial_order,
    const method_profiles::MethodProfiles* method_profiles,
    const MethodProfileOrderingConfig* config)
    : m_method_profiles(method_profiles),
      m_allowlisted_substrings(&config->method_sorting_allowlisted_substrings),
      m_legacy_order(config->legacy_order),
      m_min_appear_percent(config->min_appear_percent),
      m_second_min_appear_percent(config->second_min_appear_percent) {
  always_assert(m_method_profiles != nullptr);
  always_assert(m_allowlisted_substrings != nullptr);

  m_cache.reserve(initial_order.size());

  m_coldstart_start_marker = static_cast<DexMethod*>(
      DexMethod::get_method("Lcom/facebook/common/methodpreloader/primarydeps/"
                            "StartColdStartMethodPreloaderMethodMarker;"
                            ".startColdStartMethods:()V"));
  m_coldstart_end_marker = static_cast<DexMethod*>(
      DexMethod::get_method("Lcom/facebook/common/methodpreloader/primarydeps/"
                            "EndColdStartMethodPreloaderMethodMarker;"
                            ".endColdStartMethods:()V"));

  for (const auto& pair : m_method_profiles->all_interactions()) {
    std::string interaction_id = pair.first;
    if (interaction_id.empty()) {
      // For backwards compatibility. Older versions of the aggregate profiles
      // only have cold start (and no interaction_id column)
      interaction_id = COLD_START;
    }
    if (!m_legacy_order || interaction_id == COLD_START) {
      m_interactions.push_back(interaction_id);
    }
  }
  std::sort(m_interactions.begin(), m_interactions.end(),
            [this](const std::string& a, const std::string& b) {
              if (a == b) {
                return false;
              }

              // Cold Start always comes first;
              if (a == COLD_START) {
                return true;
              }
              if (b == COLD_START) {
                return false;
              }

              // Give priority to interactions that happen more often
              const auto& a_interactions =
                  m_method_profiles->get_interaction_count(a);
              const auto& b_interactions =
                  m_method_profiles->get_interaction_count(b);
              if (a_interactions != boost::none &&
                  b_interactions != boost::none) {
                return *a_interactions > *b_interactions;
              }

              // fall back to alphabetical
              return a < b;
            });

  for (auto method : initial_order) {
    m_initial_order.emplace(method, m_initial_order.size());
  }
}

double dexmethods_profiled_comparator::get_method_sort_num(
    const DexMethod* method) {
  double range_begin = 0.0;

  std::optional<double> secondary_ordering = std::nullopt;

  for (const auto& interaction_id : m_interactions) {
    if (interaction_id == COLD_START && m_coldstart_start_marker != nullptr &&
        m_coldstart_end_marker != nullptr) {
      if (method == m_coldstart_start_marker) {
        return range_begin;
      } else if (method == m_coldstart_end_marker) {
        return range_begin + RANGE_SIZE;
      }
    }
    const auto& stats_map = m_method_profiles->method_stats(interaction_id);
    auto it = stats_map.find(method);
    if (it != stats_map.end()) {
      const auto& stat = it->second;
      if (m_legacy_order) {
        if (stat.appear_percent >= 95.0) {
          return range_begin + RANGE_SIZE / 2;
        } else {
          continue;
        }
      }

      auto mixed_ordering = [](double appear_percent, double appear_bias,
                               double order_percent, double order_multiplier) {
        // Prefer high appearance percents and low order percents. This
        // intentionally doesn't strictly order methods by appear_percent then
        // order_percent, rather both values are used and with greater weight
        // given to appear_percent.
        double raw_score =
            (appear_bias - appear_percent) + order_multiplier * order_percent;
        return std::min(100.0, std::max(raw_score, 0.0));
      };

      if (stat.appear_percent >= m_min_appear_percent) {
        double score =
            mixed_ordering(stat.appear_percent, 100.0, stat.order_percent, 0.1);
        // Reminder: Lower sort numbers come sooner in the dex file
        return range_begin + score * RANGE_SIZE / 100.0;
      }

      if (stat.appear_percent >= m_second_min_appear_percent) {
        if (!secondary_ordering) {
          double score =
              mixed_ordering(stat.appear_percent, m_min_appear_percent,
                             stat.order_percent, 0.1);

          secondary_ordering = RANGE_STRIDE * m_interactions.size() +
                               range_begin + score * RANGE_SIZE / 100.0;
        }
        continue;
      }
    }
    range_begin += RANGE_STRIDE;
  }

  if (secondary_ordering) {
    return *secondary_ordering;
  }

  // If the method is not present in profiled order file we'll put it in the
  // end of the code section
  return VERY_END;
}

double dexmethods_profiled_comparator::get_method_sort_num_override(
    const DexMethod* method) {
  const std::string& deobfname = method->get_deobfuscated_name_or_empty();
  for (const std::string& substr : *m_allowlisted_substrings) {
    if (deobfname.find(substr) != std::string::npos) {
      return COLD_START_RANGE_BEGIN + RANGE_SIZE / 2;
    }
  }
  return VERY_END;
}

double dexmethods_profiled_comparator::get_overall_method_sort_num(
    const DexMethod* m) {
  double w = get_method_sort_num(m);
  if (w == VERY_END) {
    // For methods not included in the profiled methods file, move them to
    // the top section anyway if they match one of the allowed substrings.
    w = get_method_sort_num_override(m);
  }
  return w;
}

bool dexmethods_profiled_comparator::operator()(DexMethod* a, DexMethod* b) {
  if (a == nullptr) {
    return b != nullptr;
  } else if (b == nullptr) {
    return false;
  }

  auto get_sort_num = [this](DexMethod* m) -> double {
    const auto& search = m_cache.find(m);
    if (search != m_cache.end()) {
      return search->second;
    }

    double w = get_overall_method_sort_num(m);

    m_cache.emplace(m, w);
    return w;
  };

  double sort_num_a = get_sort_num(a);
  double sort_num_b = get_sort_num(b);

  if (sort_num_a != sort_num_b) {
    return sort_num_a < sort_num_b;
  }

  return m_initial_order.at(a) < m_initial_order.at(b);
}
