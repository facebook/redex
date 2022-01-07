/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "utils/Serialize.h"
#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/Unicode.h"
#include "utils/Vector.h"

namespace arsc {

void align_vec(size_t s, android::Vector<char>* vec) {
  size_t r = vec->size() % s;
  if (r > 0) {
    for (size_t i = s - r; i > 0; i--) {
      vec->push_back(0);
    }
  }
}

void push_short(uint16_t data, android::Vector<char>* vec) {
  auto swapped = htods(data);
  vec->push_back(swapped);
  vec->push_back(swapped >> 8);
}

void push_long(uint32_t data, android::Vector<char>* vec) {
  auto swapped = htodl(data);
  vec->push_back(swapped);
  vec->push_back(swapped >> 8);
  vec->push_back(swapped >> 16);
  vec->push_back(swapped >> 24);
}

void push_u8_length(size_t len, android::Vector<char>* vec) {
  // If len > 2^7-1, then set the most significant bit, then use a second byte
  // to describe the length (leaving 15 bits for the actual len).
  if (len >= 0x80) {
    const auto mask = 0x8000;
    LOG_FATAL_IF(len >= mask, "String length too large");
    // Set the high bit, then push it in two pieces (can't just push short).
    uint16_t encoded = mask | len;
    uint8_t high = encoded >> 8;
    uint8_t low = encoded & 0xFF;
    vec->push_back(high);
    vec->push_back(low);
  } else {
    vec->push_back((char)len);
  }
}

namespace {
void encode_string8(const char* string,
                    size_t& len,
                    android::Vector<char>* vec) {
  // aapt2 writes both the utf16 length followed by utf8 length
  auto u16_len = utf8_to_utf16_length((const uint8_t*)string, len);
  push_u8_length(u16_len, vec);
  push_u8_length(len, vec);
  // Push each char
  for (auto c = (char*)string; *c; c++) {
    vec->push_back(*c);
  }
  vec->push_back('\0');
}

void encode_string16(const char16_t* s,
                     size_t len,
                     android::Vector<char>* vec) {
  // Push uint16_t (2 bytes) describing the length. If length > 2^15-1, then set
  // most significant bit, then use two uint16_t to describe the length (first
  // uint16_t will be the high word).
  if (len >= 0x8000) {
    const auto mask = 0x80000000;
    LOG_FATAL_IF(len >= mask, "String length too large");
    uint32_t encoded = mask | len;
    push_short(encoded >> 16, vec);
    push_short(encoded & 0xFFFF, vec);
  } else {
    push_short((uint16_t)len, vec);
  }
  for (uint16_t* c = (uint16_t*)s; *c; c++) {
    push_short(*c, vec);
  }
  push_short('\0', vec);
}
} // namespace

void encode_string8(const android::String8& s, android::Vector<char>* vec) {
  auto len = s.size();
  encode_string8(s.string(), len, vec);
}

void encode_string16(const android::String16& s, android::Vector<char>* vec) {
  encode_string16(s.string(), s.size(), vec);
}
} // namespace arsc
