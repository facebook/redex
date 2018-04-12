/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "QuickData.h"
#include "file-utils.h"

namespace {

uint32_t size_of_header() {
  return sizeof(uint32_t) // dexes no
         + sizeof(uint32_t); // dex_identifiers_offset
}

uint32_t size_of_dex_info(uint32_t num_dexes) {
  return num_dexes * ( // DexInfo
                         sizeof(uint32_t) // size of field_offsets
                         + sizeof(uint32_t) // field_offsets start offset
                     );
}

uint32_t size_of_field_offsets(uint32_t total_fields) {
  return total_fields * (sizeof(uint16_t) // offset
                        );
}

uint32_t calculate_dex_identifiers_offset(uint32_t num_dexes,
                                          uint32_t total_fields) {
  return size_of_header() + size_of_dex_info(num_dexes) +
         size_of_field_offsets(total_fields);
}

uint32_t calculate_total_fields(
    std::map<std::string, uint32_t>& dex_to_field_offset_size) {
  uint32_t result = 0;
  for (const auto& pair : dex_to_field_offset_size) {
    result += pair.second;
  }
  return result;
}

} // namespace

void QuickData::add_field_offset(const std::string& dex,
                                 const uint32_t field_idx,
                                 const uint16_t offset) {
  if (dex_to_field_idx_to_offset.count(dex) == 0) {
    std::unordered_map<uint32_t, uint16_t> dex_map;
    dex_to_field_idx_to_offset[dex] = std::move(dex_map);
    dex_to_field_offset_size[dex] = 0;
  }
  dex_to_field_idx_to_offset[dex][field_idx] = offset;
  if (field_idx + 1 > dex_to_field_offset_size[dex]) {
    dex_to_field_offset_size[dex] = field_idx + 1;
  }
}

uint16_t QuickData::get_field_offset(const std::string& dex,
                                     const uint32_t field_idx) {
  return 0;
}

void QuickData::serialize(std::shared_ptr<FILE*> fd) {

  {
  auto data_fh = FileHandle(*fd);
  uint32_t num_dexes = dex_to_field_idx_to_offset.size();
  write_word(data_fh, num_dexes);

  uint32_t total_fields = calculate_total_fields(dex_to_field_offset_size);
  uint32_t dex_identifiers_offset =
      calculate_dex_identifiers_offset(num_dexes, total_fields);
  write_word(data_fh,
             calculate_dex_identifiers_offset(num_dexes, total_fields));

  CHECK(data_fh.bytes_written() == size_of_header());

  uint32_t next_field_offset = size_of_header() + size_of_dex_info(num_dexes);
  for (const auto& pair : dex_to_field_offset_size) {
    write_word(data_fh, pair.second);
    write_word(data_fh, next_field_offset);
    next_field_offset += pair.second * sizeof(uint16_t);
  }
  CHECK(data_fh.bytes_written() ==
        size_of_header() + size_of_dex_info(num_dexes));

  for (const auto& pair : dex_to_field_offset_size) {
    auto& field_offset_map = dex_to_field_idx_to_offset[pair.first];
    for (uint32_t field_idx = 0; field_idx < pair.second; ++field_idx) {
      if (field_offset_map.count(field_idx) > 0) {
        write_short(data_fh, field_offset_map[field_idx]);
      } else {
        write_short(data_fh, 0);
      }
    }
    CHECK(data_fh.bytes_written() == size_of_header() +
                                         size_of_dex_info(num_dexes) +
                                         pair.second * sizeof(uint16_t));
  }

  CHECK(data_fh.bytes_written() == dex_identifiers_offset);

  for (const auto& pair : dex_to_field_offset_size) {
    write_word(data_fh, pair.first.size());
    write_str(data_fh, pair.first);
  }

  } // FileHandle lifecycle

  fd = nullptr;
}

QuickData::QuickData(FILE* fd) {}
