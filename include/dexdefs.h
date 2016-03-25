/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "Debug.h"

/*
 * The the dex spec can be found here:
 * https://source.android.com/devices/tech/dalvik/dex-format.html
 *
 * The values here can be found within the spec.  Naming is
 * kept close enough so that you should be able to search
 * the spec for the variable name.
 *
 */

#define DEX_HEADER_DEXMAGIC "dex\n035"
#define ENDIAN_CONSTANT (0x12345678)

#define TYPE_HEADER_ITEM             (0x0000)
#define TYPE_STRING_ID_ITEM          (0x0001)
#define TYPE_TYPE_ID_ITEM            (0x0002)
#define TYPE_PROTO_ID_ITEM           (0x0003)
#define TYPE_FIELD_ID_ITEM           (0x0004)
#define TYPE_METHOD_ID_ITEM          (0x0005)
#define TYPE_CLASS_DEF_ITEM          (0x0006)
#define TYPE_MAP_LIST                (0x1000)
#define TYPE_TYPE_LIST               (0x1001)
#define TYPE_ANNOTATION_SET_REF_LIST (0x1002)
#define TYPE_ANNOTATION_SET_ITEM     (0x1003)
#define TYPE_CLASS_DATA_ITEM         (0x2000)
#define TYPE_CODE_ITEM               (0x2001)
#define TYPE_STRING_DATA_ITEM        (0x2002)
#define TYPE_DEBUG_INFO_ITEM         (0x2003)
#define TYPE_ANNOTATION_ITEM         (0x2004)
#define TYPE_ENCODED_ARRAY_ITEM      (0x2005)
#define TYPE_ANNOTATIONS_DIR_ITEM    (0x2006)

/*
 * LEB128 is a DEX data type.  It was borrowed by DEX from the DWARF3
 * specification.  Dex uses a subset of it, which it uses for encoding of
 * both signed and unsigned 32bit values.  The reason DEX uses it is to
 * take up less space in a binary file for numbers which tend to be small.
 *
 * We are only using uleb128 encoded data from ClassDef's.  ClassDef's do
 * not contain signed encoded data (sleb128's), so we only include an
 * implementation of uleb128.
 *
 * For more detailed information please consult the Dalvik Executable
 * Format documentation.
 *
 * Link here:
 * https://source.android.com/devices/tech/dalvik/dex-format.html
 */
/* read_uleb128:
 * Returns the uint32_t encoded at the pointed to memory.  Also
 * advances the pointer to the next uleb128.
 */
inline uint32_t read_uleb128(const uint8_t** _ptr) {
  const uint8_t* ptr = *_ptr;
  int result = *(ptr++);

  if (result > 0x7f) {
    int cur = *(ptr++);
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur > 0x7f) {
      cur = *(ptr++);
      result |= (cur & 0x7f) << 14;
      if (cur > 0x7f) {
        cur = *(ptr++);
        result |= (cur & 0x7f) << 21;
        if (cur > 0x7f) {
          cur = *(ptr++);
          result |= cur << 28;
        }
      }
    }
  }
  *_ptr = ptr;
  return result;
}

inline uint32_t read_uleb128p1(const uint8_t** _ptr) {
  int v = read_uleb128(_ptr);
  return (v - 1);
}

/*
 * Number of bytes it takes to encode a particular integer in a uleb128.
 */
inline int uleb128_encoding_size(uint32_t v) {
  v >>= 7;
  if (v == 0) return 1;
  v >>= 7;
  if (v == 0) return 2;
  v >>= 7;
  if (v == 0) return 3;
  v >>= 7;
  if (v == 0) return 4;
  return 5;
}

inline int32_t read_sleb128(const uint8_t** _ptr) {
  const uint8_t* ptr = *_ptr;
  int32_t result = *(ptr++);

  if (result <= 0x7f) {
    result = (result << 25) >> 25;
  } else {
    int cur = *(ptr++);
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur <= 0x7f) {
      result = (result << 18) >> 18;
    } else {
      cur = *(ptr++);
      result |= (cur & 0x7f) << 14;
      if (cur <= 0x7f) {
        result = (result << 11) >> 11;
      } else {
        cur = *(ptr++);
        result |= (cur & 0x7f) << 21;
        if (cur <= 0x7f) {
          result = (result << 4) >> 4;
        } else {
          cur = *(ptr++);
          result |= cur << 28;
        }
      }
    }
  }
  *_ptr = ptr;
  return result;
}

/* write_uleb128
 * Encode the uint32_t val at the output referred to by ptr.  Returns the
 * pointer to the next location for encoding.
 */
inline uint8_t* write_uleb128(uint8_t* ptr, uint32_t val) {
  while (1) {
    uint8_t v = val & 0x7f;
    if (v != val) {
      *ptr++ = v | 0x80;
      val >>= 7;
    } else {
      *ptr++ = v;
      return ptr;
    }
  }
}

inline uint8_t* write_uleb128p1(uint8_t* ptr, uint32_t val) {
  return write_uleb128(ptr, val + 1);
}

inline uint8_t* write_sleb128(uint8_t* ptr, int32_t val) {
  while (1) {
    uint8_t v = val & 0x7f;
    if (v == val) {
      /* Positive sleb termination */
      if (v & 0x40) {
        /* Can't let it sign extend... */
        *ptr++ = v | 0x80;
        *ptr++ = 0;
        return ptr;
      }
      *ptr++ = v;
      return ptr;
    }
    if (val < 0 && val > -64) {
      /* Negative sleb termination */
      *ptr++ = v;
      return ptr;
    }
    *ptr++ = v | 0x80;
    val >>= 7;
  }
}

inline uint32_t mutf8_next_code_point(const char*& s) {
  uint8_t v = *s++;
  /* Simple common case first, a utf8 char... */
  if (!(v & 0x80)) return v;
  uint8_t v2 = *s++;
  if ((v2 & 0xc0) != 0x80) {
    /* Invalid string. */
    always_assert_log(false, "Invalid 2nd byte on mutf8 string");
  }
  /* Two byte code point */
  if ((v & 0xe0) == 0xc0) {
    return (v & 0x1f) << 6 | (v2 & 0x3f);
  }
  /* Three byte code point */
  if ((v & 0xf0) == 0xe0) {
    uint8_t v3 = *s++;
    if ((v2 & 0xc0) != 0x80) {
      /* Invalid string. */
      always_assert_log(false, "Invalid 3rd byte on mutf8 string");
    }
    return (v & 0x1f) << 12 | (v2 & 0x3f) << 6 | (v3 & 0x3f);
  }
  /* Invalid string. */
  always_assert_log(false, "Invalid size encoding mutf8 string");
}
/*
 * This header exists at the beginning of a non-optimized dex.  The checking
 * we do on this has to do with making sure we're working on a non-opt
 * dex.  See link to Dalvik Executable Format above.
 */
struct __attribute__((packed)) dex_header {
  char magic[8];
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
};

struct __attribute__((packed)) dex_string_id {
  uint32_t offset;
};

struct __attribute__((packed)) dex_type_id {
  uint32_t string_idx;
};

struct __attribute__((packed)) dex_map_item {
  uint16_t type;
  uint16_t na /* Not used */;
  uint32_t size /* Item count, not byte size */;
  uint32_t offset /* From start of file */;
};
#define DEX_NO_INDEX (0xffffffff)
struct __attribute__((packed)) dex_class_def {
  uint32_t typeidx;
  uint32_t access_flags;
  uint32_t super_idx;
  uint32_t interfaces_off;
  uint32_t source_file_idx;
  uint32_t annotations_off;
  uint32_t class_data_offset;
  uint32_t static_values_off;
};
struct __attribute__((packed)) dex_method_id {
  uint16_t classidx;
  uint16_t protoidx;
  uint32_t nameidx;
};
struct __attribute__((packed)) dex_field_id {
  uint16_t classidx;
  uint16_t typeidx;
  uint32_t nameidx;
};
struct __attribute__((packed)) dex_proto_id {
  uint32_t shortyidx;
  uint32_t rtypeidx;
  uint32_t param_off;
};

struct __attribute__((packed)) dex_code_item {
  uint16_t registers_size;
  uint16_t ins_size;
  uint16_t outs_size;
  uint16_t tries_size;
  uint32_t debug_info_off;
  uint32_t insns_size;
};

struct __attribute__((packed)) dex_tries_item {
  uint32_t start_addr;
  uint16_t insn_count;
  uint16_t handler_off;
};

struct __attribute__((packed)) dex_annotations_directory_item {
  uint32_t class_annotations_off;
  uint32_t fields_size;
  uint32_t methods_size;
  uint32_t parameters_size;
};

enum DexDebugItemOpcode {
  DBG_END_SEQUENCE         = 0x00,
  DBG_ADVANCE_PC           = 0x01,
  DBG_ADVANCE_LINE         = 0x02,
  DBG_START_LOCAL          = 0x03,
  DBG_START_LOCAL_EXTENDED = 0x04,
  DBG_END_LOCAL            = 0x05,
  DBG_RESTART_LOCAL        = 0x06,
  DBG_SET_PROLOGUE_END     = 0x07,
  DBG_SET_EPILOGUE_BEGIN   = 0x08,
  DBG_SET_FILE             = 0x09,
  DBG_LAST_SPECIAL_OPCODE  = 0xff
};
