/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OatmealUtil.h"
#include "memory-accounter.h"

#include <cstring>
#include <functional>
#include <vector>

using VdexChecksum = uint32_t;

constexpr uint32_t kDexMagicNum = 0x0a786564;

struct PACK DexClassDef {
  uint16_t class_idx;
  uint16_t pad1;
  uint32_t access_flags;
  uint16_t superclass_idx;
  uint16_t pad2;
  uint32_t interfaces_off;
  uint32_t source_file_idx;
  uint32_t annotations_off;
  uint32_t class_data_off;
  uint32_t static_values_off;
};

// Header for dex files. Note that this currently consumes the entire
// contents of the dex file (in addition to the header proper) for the
// purposes of memory-accounting.
struct PACK DexFileHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t checksum;
  uint8_t signature[20];
  uint32_t file_size;
  uint32_t header_size;
  uint32_t endian_tag;
  uint32_t link_size;

  uint32_t link_off;
  uint32_t map_off;
  uint32_t string_ids_size;
  uint32_t string_ids_off;
  uint32_t type_ids_size;
  uint32_t type_ids_off;
  uint32_t proto_ids_size;
  uint32_t proto_ids_off;
  uint32_t field_ids_size;
  uint32_t field_ids_off;
  uint32_t method_ids_size;
  uint32_t method_ids_off;
  uint32_t class_defs_size;
  uint32_t class_defs_off;
  uint32_t data_size;
  uint32_t data_off;

  static DexFileHeader parse(ConstBuffer buf) {
    DexFileHeader header;
    memcpy(&header, buf.ptr, sizeof(DexFileHeader));

    // Mark the whole file consumed.
    cur_ma()->markRangeConsumed(buf.ptr, header.file_size);

    return header;
  }
};

struct PACK MethodId {
  uint16_t class_idx; // index into type_ids_ array for defining class
  uint16_t proto_idx; // index into proto_ids_ array for method prototype
  uint32_t name_idx; // index into string_ids_ array for method name
};

class QuickData;

void quicken_dex(const char* location,
                 const QuickData* quick_data,
                 FileHandle& out);

class stream {
 public:
  // This is a "static class". Disallow construction.
  stream() = delete;
  ~stream() = delete;

  using InsnWalkerFn =
      const std::function<void(DexOpcode, const uint16_t* const ptr)>&;
  using CodeItemWalkerFn = const std::function<void(const uint8_t* const ptr)>&;

  static void stream_dex(const uint8_t* begin,
                         const size_t size,
                         InsnWalkerFn walker,
                         CodeItemWalkerFn code_item_walker =
                             [](const uint8_t* const insn) {});
};

void print_dex_opcodes(const uint8_t* begin, const size_t size);
