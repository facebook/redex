/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "QuickData.h"
#include "file-utils.h"
#include "mmap.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

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
                                     const uint32_t field_idx) const {
  if (dex_to_field_idx_to_offset.count(dex) > 0 &&
      dex_to_field_idx_to_offset.at(dex).count(field_idx) > 0) {
    return dex_to_field_idx_to_offset.at(dex).at(field_idx);
  }

  return 0;
}

void QuickData::serialize(const std::shared_ptr<FILE*>& fd) {

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

    uint32_t current_total_fields = 0;
    for (const auto& pair : dex_to_field_offset_size) {
      auto& field_offset_map = dex_to_field_idx_to_offset[pair.first];
      for (uint32_t field_idx = 0; field_idx < pair.second; ++field_idx) {
        if (field_offset_map.count(field_idx) > 0) {
          write_short(data_fh, field_offset_map[field_idx]);
        } else {
          write_short(data_fh, 0);
        }
      }

      current_total_fields += pair.second;
      CHECK(data_fh.bytes_written() ==
            size_of_header() + size_of_dex_info(num_dexes) +
                current_total_fields * sizeof(uint16_t));
    }

    CHECK(data_fh.bytes_written() == dex_identifiers_offset);

    for (const auto& pair : dex_to_field_offset_size) {
      write_word(data_fh, pair.first.size());
      write_str(data_fh, pair.first);
    }

  } // FileHandle lifecycle

  *fd = nullptr;
}

QuickData::QuickData(const char* location) { load_data(location); }

void QuickData::load_data(const char* location) {
  FILE* fd = fopen(location, "r");

  std::string error_msg;
  struct stat sbuf;
  std::memset(&sbuf, 0, sizeof(sbuf));

  if (fstat(fileno(fd), &sbuf) == -1) {
    throw std::runtime_error("QuickData: fstat failed");
  }
  if (S_ISDIR(sbuf.st_mode)) {
    throw std::runtime_error("QuickData: Attempt to mmap a directory");
  }
  size_t length = sbuf.st_size;

  std::unique_ptr<MappedFile> file;
  file.reset(MappedFile::mmap_file(
      length, PROT_READ, MAP_PRIVATE, fileno(fd), location, &error_msg));
  fclose(fd);
  if (file == nullptr) {
    CHECK(!error_msg.empty());
    std::ostringstream error_str;
    error_str << "QuickData: Error attempting to mmap " << error_msg.c_str();
    throw std::runtime_error(error_str.str());
  }

  const Header* header = reinterpret_cast<Header*>(file->begin());

  // printf("Header: %u, %u\n", header->dexes_num,
  // header->dex_identifiers_offset);

  const uint8_t* current_dex_identifier =
      file->begin() + header->dex_identifiers_offset;
  const uint8_t* current_dex_info = file->begin() + size_of_header();

  for (size_t dex_idx = 0; dex_idx < header->dexes_num; ++dex_idx) {
    uint32_t identifier_size = *current_dex_identifier;
    std::string dex_identifier(reinterpret_cast<const char*>(
                                   current_dex_identifier + sizeof(uint32_t)),
                               identifier_size);

    const DexInfo* dex_info =
        reinterpret_cast<const DexInfo*>(current_dex_info);

    // printf("Dex: %s\n", dex_identifier.c_str());
    const uint16_t* current_field_offsets = reinterpret_cast<const uint16_t*>(
        file->begin() + dex_info->field_offsets_off);

    for (uint32_t field_idx = 0; field_idx < dex_info->field_offsets_size;
         ++field_idx) {
      add_field_offset(
          dex_identifier, field_idx, *(current_field_offsets + field_idx));
      // printf("%u -> %u\n", field_idx, *(current_field_offsets + field_idx));
    }

    current_dex_identifier += sizeof(uint32_t) + identifier_size;
    current_dex_info += sizeof(DexInfo);
  }
}
