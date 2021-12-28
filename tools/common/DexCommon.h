/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexAccess.h"
#include "DexDefs.h"
#include <stdint.h>

using symdstr = uint32_t; // Offset into symtool string table.

struct ddump_data {
  ssize_t dexorder_size;
  ssize_t dex_size;
  char* dexmmap;
  char* dexordermmap;
  dex_header* dexh;
  char* strtab;
  symdstr* strings;
  dex_string_id* dex_string_ids;
  dex_class_def* dex_class_defs;
  dex_field_id* dex_field_ids;
  dex_method_id* dex_method_ids;
  dex_proto_id* dex_proto_ids;
  const char* dex_filename;
};

void get_dex_map_items(ddump_data* rd,
                       unsigned int* _count,
                       dex_map_item** _maps);
dex_map_item* get_dex_map_item(ddump_data* rd, uint16_t type);
void open_dex_file(const char* filename, ddump_data* rd);
void get_type_extent(ddump_data* rd,
                     uint16_t type,
                     uint32_t& start,
                     uint32_t& end);
void get_code_extent(ddump_data* rd, uint32_t& codestart, uint32_t& codeend);
void get_class_data_extent(ddump_data* rd, uint32_t& start, uint32_t& end);

char* dex_raw_string_by_idx(ddump_data* rd, uint32_t idx);
char* dex_string_by_idx(ddump_data* rd, uint32_t idx);
char* dex_string_by_type_idx(ddump_data* rd, uint16_t typeidx);
char* find_string_in_dex(ddump_data* rd, const char* string, uint32_t* idx);
bool find_typeid_for_idx(ddump_data* rd, uint32_t idx, uint16_t* typeidx);
