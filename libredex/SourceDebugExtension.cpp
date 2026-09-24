/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceDebugExtension.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace source_debug_extension {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool is_whitespace(char value) { return value == ' ' || value == '\t'; }

std::string_view trim(std::string_view value) {
  while (!value.empty() && is_whitespace(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_whitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<uint32_t> parse_uint(std::string_view value) {
  value = trim(value);
  if (value.empty()) {
    return std::nullopt;
  }
  uint32_t parsed;
  const auto [ptr, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc() || ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

std::vector<std::string_view> split_lines(std::string_view value) {
  std::vector<std::string_view> lines;
  while (!value.empty()) {
    auto end = value.find('\n');
    auto line = value.substr(0, end);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.push_back(line);
    if (end == std::string_view::npos) {
      break;
    }
    value.remove_prefix(end + 1);
  }
  return lines;
}

template <typename Stratum>
bool parse_file(std::string_view line,
                std::optional<std::string_view> path,
                Stratum& stratum) {
  if (!line.empty() && line.front() == '+') {
    line = trim(line.substr(1));
  }
  auto separator = line.find_first_of(" \t");
  if (separator == std::string_view::npos) {
    return false;
  }
  auto id = parse_uint(line.substr(0, separator));
  auto name = trim(line.substr(separator + 1));
  if (!id || name.empty() || (path && path->empty())) {
    return false;
  }
  // DexPosition stores the SMAP FileName. The optional AbsoluteFileName is
  // syntax-validated above but intentionally not retained.
  if (!stratum.files.emplace(*id, name).second) {
    return false;
  }
  if (!stratum.first_file_id) {
    stratum.first_file_id = id;
  }
  return true;
}

template <typename Stratum>
bool parse_line(std::string_view line,
                uint32_t& current_file_id,
                Stratum& stratum) {
  auto colon = line.find(':');
  if (colon == std::string_view::npos ||
      line.find(':', colon + 1) != std::string_view::npos) {
    return false;
  }
  auto input = line.substr(0, colon);
  auto output = line.substr(colon + 1);

  uint32_t repeat_count = 1;
  auto input_comma = input.find(',');
  if (input_comma != std::string_view::npos) {
    if (auto parsed = parse_uint(input.substr(input_comma + 1))) {
      repeat_count = *parsed;
    } else {
      return false;
    }
    input = input.substr(0, input_comma);
  }

  uint32_t file_id = current_file_id;
  auto file_separator = input.find('#');
  if (file_separator != std::string_view::npos) {
    if (auto parsed = parse_uint(input.substr(file_separator + 1))) {
      file_id = *parsed;
    } else {
      return false;
    }
    input = input.substr(0, file_separator);
  }

  uint32_t output_increment = 1;
  auto output_comma = output.find(',');
  if (output_comma != std::string_view::npos) {
    if (auto parsed = parse_uint(output.substr(output_comma + 1))) {
      output_increment = *parsed;
    } else {
      return false;
    }
    output = output.substr(0, output_comma);
  }
  auto input_start = parse_uint(input);
  auto output_start = parse_uint(output);
  if (!input_start || !output_start || repeat_count == 0 ||
      output_increment == 0 || stratum.files.count(file_id) == 0) {
    return false;
  }

  const uint64_t output_count =
      static_cast<uint64_t>(repeat_count) * output_increment;
  if (static_cast<uint64_t>(*output_start) + output_count - 1 >
          std::numeric_limits<uint32_t>::max() ||
      static_cast<uint64_t>(*input_start) + repeat_count - 1 >
          std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  current_file_id = file_id;
  stratum.lines.push_back(
      {*input_start, file_id, repeat_count, *output_start, output_increment});
  return true;
}

} // namespace

std::optional<SourceDebugExtension> SourceDebugExtension::parse(
    std::string_view value) {
  auto lines = split_lines(value);
  if (lines.size() < 4 || lines[0] != "SMAP" || lines[1].empty() ||
      lines[2].empty()) {
    return std::nullopt;
  }

  SourceDebugExtension result;
  const auto default_stratum_name = lines[2];
  Stratum* stratum = nullptr;
  bool found_end = false;
  for (size_t index = 3; index < lines.size();) {
    auto line = lines[index++];
    auto directive = trim(line);
    if (directive == "*E") {
      found_end = true;
      break;
    }
    if (starts_with(directive, "*S") && directive.size() > 2 &&
        is_whitespace(directive[2])) {
      auto name = trim(directive.substr(2));
      stratum = name == default_stratum_name
                    ? &result.m_default_stratum
                    : (name == "KotlinDebug" ? &result.m_kotlin_debug_stratum
                                             : nullptr);
      continue;
    }
    if (directive == "*F") {
      while (index < lines.size() && !starts_with(lines[index], "*")) {
        auto file = lines[index++];
        if (trim(file).empty()) {
          continue;
        }
        bool has_path = !file.empty() && file.front() == '+';
        std::optional<std::string_view> path;
        if (has_path) {
          if (index >= lines.size() || starts_with(lines[index], "*")) {
            return std::nullopt;
          }
          path = lines[index++];
        }
        if (stratum != nullptr && !parse_file(file, path, *stratum)) {
          return std::nullopt;
        }
      }
      continue;
    }
    if (directive == "*L") {
      if (stratum == nullptr) {
        while (index < lines.size() && !starts_with(lines[index], "*")) {
          index++;
        }
        continue;
      }
      uint32_t current_file_id = stratum->first_file_id.value_or(0);
      while (index < lines.size() && !starts_with(lines[index], "*")) {
        if (!trim(lines[index]).empty() &&
            !parse_line(lines[index], current_file_id, *stratum)) {
          return std::nullopt;
        }
        index++;
      }
      continue;
    }
    if (!directive.empty() && directive.front() == '*') {
      while (index < lines.size() && !starts_with(lines[index], "*")) {
        index++;
      }
      continue;
    }
    if (!directive.empty()) {
      return std::nullopt;
    }
  }

  if (!found_end || result.m_default_stratum.lines.empty()) {
    return std::nullopt;
  }
  auto prepare = [](Stratum* stratum) {
    std::sort(stratum->lines.begin(), stratum->lines.end(),
              [](const auto& first, const auto& second) {
                return first.output_start < second.output_start;
              });
    uint64_t previous_end = 0;
    for (const auto& mapping : stratum->lines) {
      if (mapping.output_start < previous_end) {
        return false;
      }
      previous_end = static_cast<uint64_t>(mapping.output_start) +
                     static_cast<uint64_t>(mapping.repeat_count) *
                         mapping.output_increment;
    }
    return true;
  };
  if (!prepare(&result.m_default_stratum) ||
      !prepare(&result.m_kotlin_debug_stratum)) {
    return std::nullopt;
  }
  return result;
}

std::optional<size_t> SourceDebugExtension::find_mapping_index(
    const Stratum& stratum, uint32_t output_line) {
  auto mapping =
      std::upper_bound(stratum.lines.begin(), stratum.lines.end(), output_line,
                       [](uint32_t line, const auto& candidate) {
                         return line < candidate.output_start;
                       });
  if (mapping == stratum.lines.begin()) {
    return std::nullopt;
  }
  --mapping;
  auto offset = static_cast<uint64_t>(output_line) - mapping->output_start;
  auto output_count =
      static_cast<uint64_t>(mapping->repeat_count) * mapping->output_increment;
  if (offset >= output_count) {
    return std::nullopt;
  }
  return static_cast<size_t>(mapping - stratum.lines.begin());
}

std::optional<SourcePosition> SourceDebugExtension::map(const Stratum& stratum,
                                                        uint32_t output_line) {
  auto mapping_index = find_mapping_index(stratum, output_line);
  if (!mapping_index) {
    return std::nullopt;
  }
  return map(stratum, *mapping_index, output_line);
}

std::optional<SourcePosition> SourceDebugExtension::map(const Stratum& stratum,
                                                        size_t mapping_index,
                                                        uint32_t output_line) {
  const auto& mapping = stratum.lines.at(mapping_index);
  if (output_line < mapping.output_start) {
    return std::nullopt;
  }
  auto offset = static_cast<uint64_t>(output_line) - mapping.output_start;
  auto output_count =
      static_cast<uint64_t>(mapping.repeat_count) * mapping.output_increment;
  if (offset >= output_count) {
    return std::nullopt;
  }
  auto file = stratum.files.find(mapping.file_id);
  if (file == stratum.files.end()) {
    return std::nullopt;
  }
  return SourcePosition{
      file->second,
      mapping.input_start +
          static_cast<uint32_t>(offset / mapping.output_increment)};
}

std::optional<MappedPosition> SourceDebugExtension::map(
    uint32_t output_line) const {
  auto source_mapping_index =
      find_mapping_index(m_default_stratum, output_line);
  if (!source_mapping_index) {
    return std::nullopt;
  }
  auto source = map(m_default_stratum, *source_mapping_index, output_line);
  if (!source) {
    return std::nullopt;
  }

  std::vector<SourcePosition> callers;
  auto callsite_mapping_index =
      find_mapping_index(m_kotlin_debug_stratum, output_line);
  if (callsite_mapping_index) {
    auto kotlin_mapping_index = *callsite_mapping_index;
    auto callsite =
        map(m_kotlin_debug_stratum, kotlin_mapping_index, output_line);
    auto mapping_index = *source_mapping_index;
    auto output_start = m_default_stratum.lines.at(mapping_index).output_start;
    // Kotlin emits contiguous default-stratum ranges for nested inline levels,
    // with each range sharing the same final KotlinDebug call site.
    while (callsite && mapping_index > 0) {
      const auto& previous = m_default_stratum.lines.at(mapping_index - 1);
      auto previous_end = static_cast<uint64_t>(previous.output_start) +
                          static_cast<uint64_t>(previous.repeat_count) *
                              previous.output_increment;
      if (previous_end != output_start) {
        break;
      }
      while (
          kotlin_mapping_index > 0 &&
          m_kotlin_debug_stratum.lines.at(kotlin_mapping_index).output_start >
              previous.output_start) {
        kotlin_mapping_index--;
      }
      auto previous_callsite = map(m_kotlin_debug_stratum, kotlin_mapping_index,
                                   previous.output_start);
      if (!previous_callsite || previous_callsite->file != callsite->file ||
          previous_callsite->line != callsite->line) {
        break;
      }
      auto caller =
          map(m_default_stratum, mapping_index - 1, previous.output_start);
      if (!caller) {
        break;
      }
      callers.push_back(std::move(*caller));
      output_start = previous.output_start;
      mapping_index--;
    }
    if (callsite) {
      callers.push_back(std::move(*callsite));
    }
  }
  return MappedPosition{std::move(*source), std::move(callers)};
}

} // namespace source_debug_extension
