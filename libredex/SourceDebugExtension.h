/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace source_debug_extension {

struct SourcePosition {
  std::string file;
  uint32_t line;
};

struct MappedPosition {
  SourcePosition source;
  std::vector<SourcePosition> callers;
};

class SourceDebugExtension {
 public:
  static std::optional<SourceDebugExtension> parse(std::string_view value);

  std::optional<MappedPosition> map(uint32_t output_line) const;

 private:
  struct LineMapping {
    uint32_t input_start;
    uint32_t file_id;
    uint32_t repeat_count;
    uint32_t output_start;
    uint32_t output_increment;
  };

  struct Stratum {
    std::map<uint32_t, std::string> files;
    std::optional<uint32_t> first_file_id;
    std::vector<LineMapping> lines;
  };

  static std::optional<size_t> find_mapping_index(const Stratum& stratum,
                                                  uint32_t output_line);
  static std::optional<SourcePosition> map(const Stratum& stratum,
                                           uint32_t output_line);
  static std::optional<SourcePosition> map(const Stratum& stratum,
                                           size_t mapping_index,
                                           uint32_t output_line);

  Stratum m_default_stratum;
  Stratum m_kotlin_debug_stratum;
};

} // namespace source_debug_extension
