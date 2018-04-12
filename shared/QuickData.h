/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

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
 public:
  // Read Mode
  QuickData(FILE* fd);

  // Write Mode
  QuickData() {}

  void add_field_offset(const std::string& dex,
                        const uint32_t field_idx,
                        const uint16_t offset);
  uint16_t get_field_offset(const std::string& dex, const uint32_t field_idx);

  void serialize(std::shared_ptr<FILE*> fd);

 private:
  std::map<std::string, uint32_t> dex_to_field_offset_size;
  std::unordered_map<std::string, std::unordered_map<uint32_t, uint16_t>>
      dex_to_field_idx_to_offset;

  struct DexInfo {
    uint32_t field_offsets_size;
    uint32_t field_offsets_off;
  };
};
