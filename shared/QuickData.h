/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Util.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

class MappedFile;

/**
 * [Header]
 *  uint32_t number of dexes (D)
 *  uint32_t dex_identifiers_offset
 * [DexInfo] [0]
 *  uint32_t size of FieldOffsets table for this dex
 *  uint32_t start offset of FieldOffsets table for this dex
 * ...
 * [DexInfo] [D]
 * [FieldOffsets] [0]
 *  uint16_t[0]
 *  ...
 *  uint16_t[F_0]
 * ...
 * [FieldOffsets] [D]
 *  uint16_t[0]
 *  ...
 *  uint16_t[F_D]
 * [DexIdentifier] [0]
 *  uint32_t length of location (L)
 *  char[L] non zero terminated string with Canary class name for that dex
 * [DexIdentifier] [D]
 */
class QuickData {
  UNCOPYABLE(QuickData);

 public:
  // Read Mode
  explicit QuickData(const char* location);

  // Write Mode
  QuickData() = default;

  void add_field_offset(const std::string& dex,
                        const uint32_t field_idx,
                        const uint16_t offset);
  uint16_t get_field_offset(const std::string& dex,
                            const uint32_t field_idx) const;

  void serialize(const std::shared_ptr<FILE*>& fd);

 private:
  std::map<std::string, uint32_t> dex_to_field_offset_size;
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint16_t>>
      dex_to_field_idx_to_offset;

  struct Header {
    uint32_t dexes_num;
    uint32_t dex_identifiers_offset;
  };

  struct DexInfo {
    uint32_t field_offsets_size;
    uint32_t field_offsets_off;
  };

  void load_data(const char* location);
};
