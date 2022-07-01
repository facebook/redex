/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>
#include <string>

namespace dex_encoding {
namespace details {

// Hide throw details.
[[noreturn]] void throw_invalid(const char* msg);
[[noreturn]] void throw_invalid(const char* msg, uint32_t size);

} // namespace details
} // namespace dex_encoding

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
inline uint8_t uleb128_encoding_size(uint32_t v) {
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
    if (val < 0 && val >= -64) {
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
    dex_encoding::details::throw_invalid("Invalid 2nd byte on mutf8 string");
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
      dex_encoding::details::throw_invalid("Invalid 3rd byte on mutf8 string");
    }
    return (v & 0x1f) << 12 | (v2 & 0x3f) << 6 | (v3 & 0x3f);
  }
  /* Invalid string. */
  dex_encoding::details::throw_invalid("Invalid size encoding mutf8 string");
}

inline uint32_t length_of_utf8_string(const char* s) {
  if (s == nullptr) {
    return 0;
  }
  uint32_t len = 0;
  while (*s != '\0') {
    ++len;
    mutf8_next_code_point(s);
  }
  return len;
}

// https://docs.oracle.com/javase/8/docs/api/java/lang/String.html#hashCode--
inline int32_t java_hashcode_of_utf8_string(const char* s) {
  if (s == nullptr) {
    return 0;
  }

  union {
    int32_t hash;
    int64_t wide;
  } ret;

  ret.wide = 0;
  while (*s != '\0') {
    ret.wide = (ret.wide * 31 + mutf8_next_code_point(s)) & 0xFFFFFFFFll;
  }
  return ret.hash;
}

inline uint32_t size_of_utf8_char(const int32_t ival) {
  if (ival >= 0x00 && ival <= 0x7F) {
    return 1;
  } else if (ival <= 0x7FF) {
    return 2;
  } else {
    return 3;
  }
}

// Pretty much the reverse of mutf8_next_code_point().
inline std::string encode_utf8_char_to_mutf8_string(const int32_t ival) {
  uint32_t size = size_of_utf8_char(ival);
  char buf[4];
  int idx = 0;
  if (size == 1) {
    if (ival > 0x7F) {
      dex_encoding::details::throw_invalid(
          "Invalid utf8_char for encoding to mutf8 string");
    }
    if (ival == 0x00) { // \u0000 in 2 bytes
      buf[idx++] = 0xC0;
      buf[idx++] = 0x80;
    } else {
      buf[idx++] = ival;
    }
  } else if (size == 2) {
    uint8_t byte1 = 0xC0 | ((ival >> 6) & 0x1F);
    uint8_t byte2 = 0x80 | (ival & 0x3F);
    buf[idx++] = byte1;
    buf[idx++] = byte2;
  } else if (size == 3) {
    uint8_t byte1 = 0xE0 | ((ival >> 12) & 0x0F);
    uint8_t byte2 = 0x80 | ((ival >> 6) & 0x3F);
    uint8_t byte3 = 0x80 | (ival & 0x3F);
    buf[idx++] = byte1;
    buf[idx++] = byte2;
    buf[idx++] = byte3;
  } else {
    dex_encoding::details::throw_invalid("Unexpected char size: ", size);
  }

  buf[idx] = 0x00;
  return std::string(buf);
}
