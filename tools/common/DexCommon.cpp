/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexCommon.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* dex_v35_header_string = "dex\n035";
static const char* dex_v37_header_string = "dex\n037";
static const char* dex_v38_header_string = "dex\n038";

void get_dex_map_items(ddump_data* rd,
                       unsigned int* _count,
                       dex_map_item** _maps) {
  uint32_t* miptr = (uint32_t*)(rd->dexmmap + rd->dexh->map_off);
  *_count = *miptr++;
  *_maps = (dex_map_item*)miptr;
}

dex_map_item* get_dex_map_item(ddump_data* rd, uint16_t type) {
  unsigned count;
  dex_map_item* items;
  get_dex_map_items(rd, &count, &items);
  for (unsigned i = 0; i < count; ++i) {
    if (items[i].type == type) {
      return &items[i];
    }
  }

  return nullptr;
}

void open_dex_file(const char* filename, ddump_data* rd) {
  int fd = open(filename, O_RDWR);
  struct stat stat;
  rd->dex_filename = filename;
  if (fd < 0) {
    fprintf(stderr, "Cannot open dump file %s, bailing\n", filename);
    exit(1);
  }
  if (fstat(fd, &stat)) {
    fprintf(stderr, "Cannot fstat file %s, bailing\n", filename);
    exit(1);
  }
  rd->dex_size = stat.st_size;
  rd->dexmmap = (char*)mmap(nullptr,
                            rd->dex_size,
                            PROT_READ | PROT_WRITE,
                            MAP_FILE | MAP_SHARED,
                            fd,
                            0);
  close(fd);
  if (rd->dexmmap == nullptr) {
    fprintf(stderr, "Address space allocation failed for mmap, bailing\n");
    exit(1);
  }
  rd->dexh = (dex_header*)rd->dexmmap;
  if (memcmp(rd->dexh->magic, dex_v35_header_string, sizeof(rd->dexh->magic)) &&
      memcmp(rd->dexh->magic, dex_v37_header_string, sizeof(rd->dexh->magic)) &&
      memcmp(rd->dexh->magic, dex_v38_header_string, sizeof(rd->dexh->magic))) {
    fprintf(stderr, "Bad dex magic, bailing\n");
    exit(1);
  }
  rd->dex_string_ids = (dex_string_id*)(rd->dexmmap + rd->dexh->string_ids_off);
  rd->dex_class_defs = (dex_class_def*)(rd->dexmmap + rd->dexh->class_defs_off);
  rd->dex_field_ids = (dex_field_id*)(rd->dexmmap + rd->dexh->field_ids_off);
  rd->dex_method_ids = (dex_method_id*)(rd->dexmmap + rd->dexh->method_ids_off);
  rd->dex_proto_ids = (dex_proto_id*)(rd->dexmmap + rd->dexh->proto_ids_off);
}

void get_type_extent(ddump_data* rd,
                     uint16_t type,
                     uint32_t& start,
                     uint32_t& end) {
  unsigned int count;
  dex_map_item* maps;
  end = 0;
  get_dex_map_items(rd, &count, &maps);
  for (unsigned int i = 0; i < count; i++) {
    if (maps[i].type == type) {
      /* Maps size is not in bytes, which is why this is a little funky */
      start = maps[i].offset;
    }
  }
  for (unsigned int i = 0; i < count; i++) {
    if (maps[i].offset > start) {
      if (end == 0 || end > maps[i].offset) {
        end = maps[i].offset;
      }
    }
  }
}

void get_code_extent(ddump_data* rd, uint32_t& codestart, uint32_t& codeend) {
  get_type_extent(rd, TYPE_CODE_ITEM, codestart, codeend);
}

void get_class_data_extent(ddump_data* rd, uint32_t& start, uint32_t& end) {
  get_type_extent(rd, TYPE_CLASS_DATA_ITEM, start, end);
}

char* dex_raw_string_by_idx(ddump_data* rd, uint32_t idx) {
  uint32_t off = rd->dex_string_ids[idx].offset;
  return (rd->dexmmap + off);
}

char* dex_string_by_idx(ddump_data* rd, uint32_t idx) {
  char* rv = dex_raw_string_by_idx(rd, idx);
  while (*((unsigned char*)rv++) > 0x7f)
    ; /* Skip uleb128 size */
  return rv;
}

char* dex_string_by_type_idx(ddump_data* rd, uint16_t typeidx) {
  uint32_t* tptr = (uint32_t*)(rd->dexmmap + rd->dexh->type_ids_off);
  return dex_string_by_idx(rd, tptr[typeidx]);
}

bool find_typeid_for_idx(ddump_data* rd, uint32_t idx, uint16_t* typeidx) {
  uint32_t* tptr = (uint32_t*)(rd->dexmmap + rd->dexh->type_ids_off);
  for (uint32_t i = 0; i < rd->dexh->type_ids_size; i++) {
    if (*tptr++ == idx) {
      *typeidx = (uint16_t)i;
      return true;
    }
  }
  return false;
}

char* find_string_in_dex(ddump_data* rd, const char* string, uint32_t* idx) {
  int hi = rd->dexh->string_ids_size - 1;
  int lo = 0;
  int cur;
  while (hi >= lo) {
    int cmp;
    char* tocmp;
    cur = (lo + hi) / 2;
    tocmp = dex_string_by_idx(rd, cur);
    cmp = strcmp(string, tocmp);
    if (cmp > 0) {
      lo = cur + 1;
    } else if (cmp < 0) {
      hi = cur - 1;
    } else {
      *idx = cur;
      return tocmp;
    }
  }
  return nullptr;
}
