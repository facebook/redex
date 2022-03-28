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

// Just a random thing to make it easy to see (when dumping bytes) if we forgot
// to go back and correct a chunk size.
constexpr uint32_t FILL_IN_LATER = 0xEEEEEEEE;

void write_long_at_pos(size_t index,
                       uint32_t data,
                       android::Vector<char>* vec) {
  auto swapped = htodl(data);
  vec->replaceAt((char)swapped, index);
  vec->replaceAt((char)(swapped >> 8), index + 1);
  vec->replaceAt((char)(swapped >> 16), index + 2);
  vec->replaceAt((char)(swapped >> 24), index + 3);
}

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

// Does not swap byte order, just copy data as-is
void push_data_no_swap(void* data, size_t length, android::Vector<char>* out) {
  for (size_t i = 0; i < length; ++i) {
    out->push_back(*((uint8_t*)(data) + i));
  }
}

// Does not swap byte order, just copy data as-is
void push_chunk(android::ResChunk_header* header,
                android::Vector<char>* out) {
  push_data_no_swap(header, dtohl(header->size), out);
}
} // namespace

void encode_string8(const android::String8& s, android::Vector<char>* vec) {
  auto len = s.size();
  encode_string8(s.string(), len, vec);
}

void encode_string16(const android::String16& s, android::Vector<char>* vec) {
  encode_string16(s.string(), s.size(), vec);
}

void ResStringPoolBuilder::add_string(const char* s, size_t len) {
  LOG_ALWAYS_FATAL_IF(!is_utf8(), "Pool is not UTF-8");
  PtrLen<char> pair((char*)s, len);
  m_strings8.add(pair);
}

void ResStringPoolBuilder::add_style(const char* s,
                                     size_t len,
                                     SpanVector spans) {
  LOG_ALWAYS_FATAL_IF(!is_utf8(), "Pool is not UTF-8");
  LOG_ALWAYS_FATAL_IF(spans.empty(), "spans should be non-empty");
  StyleInfo<char> style = {(char*)s, len, spans};
  m_styles8.add(style);
}

void ResStringPoolBuilder::add_string(const char16_t* s, size_t len) {
  LOG_ALWAYS_FATAL_IF(is_utf8(), "Pool is not UTF-16");
  PtrLen<char16_t> pair((char16_t*)s, len);
  m_strings16.add(pair);
}

void ResStringPoolBuilder::add_style(const char16_t* s,
                                     size_t len,
                                     SpanVector spans) {
  LOG_ALWAYS_FATAL_IF(is_utf8(), "Pool is not UTF-16");
  LOG_ALWAYS_FATAL_IF(spans.empty(), "spans should be non-empty");
  StyleInfo<char16_t> style = {(char16_t*)s, len, spans};
  m_styles16.add(style);
}

void ResStringPoolBuilder::serialize(android::Vector<char>* out) {
  // NOTES ON DATA FORMAT: "styles" in this context are strings themselves with
  // additional data about HTML formatting per
  // https://developer.android.com/guide/topics/resources/string-resource#StylingWithHTML
  //
  // Consider an application defining the following two strings:
  // I like a <em>fine</em> glass of H<sub>2</sub>O in the morning!
  // Hello World.
  //
  // This will generate 4 entries in the pool, one of which will be a style
  // (we will count this as 1 style, 4 strings). The four entries will be:
  // 1) I like a fine glass of H2O in the morning!
  // 2) Hello World.
  // 3) em
  // 4) sub
  //
  // Following the actual string data, there will be two ResStringPool_span
  // structs packed sequentially, saying where the em and sub tags start/end.
  // ResStringPool_span are repeated and terminated by 0xFFFFFFFF in case there
  // are multiple spans in a single string. The list of spans (if any exist)
  // will end with a ResStringPool_span structure filled with 0xFFFFFFFF (so
  // three total):
  // https://cs.android.com/android/platform/superproject/+/android-11.0.0_r1:frameworks/base/tools/aapt2/StringPool.cpp;l=489
  //
  // Actually encoding an entry for a string itself involves writing its length
  // (which depending on the encoding might require writing the UTF-16 length
  // AND the UTF-8 length, optionally with high bit set for big lengths that
  // need more bytes to encode), then the actual string bytes, followed by a
  // null terminator. See libresource/Serialize.cpp for info on this. The
  // overall data for string entries should be padded to end on 4 byte boundary.
  //
  // ALSO NOTE:
  // String entries that have style information always come first! This is a
  // convention used to match the subsequent ResStringPool_span entries to their
  // corresponding string. Thus, all ResStringPool_span structures starting from
  // ResStringPool_header.stylesStart until an END (0xFFFFFFFF) marker belong to
  // the 0th string. Subsequent ResStringPool_span structures until another END
  // marker belong to the 1st string, and so on.
  //
  // Implementation begins by writing string data into intermediate vector. This
  // will be used to calculate offsets, and later copied to final output. While
  // we're iterating styles emitting their string data, we'll also compute the
  // size emitting the span tags will take up.
  android::Vector<uint32_t> string_idx;
  android::Vector<uint32_t> span_off;
  android::Vector<char> serialized_strings;
  auto utf8 = is_utf8();
  auto num_styles = style_count();
  // Write styles first!
  auto spans_size = 0;
  for (size_t i = 0; i < num_styles; i++) {
    string_idx.push_back(serialized_strings.size());
    span_off.push_back(spans_size);
    if (utf8) {
      auto info = m_styles8[i];
      encode_string8(info.string, info.len, &serialized_strings);
      spans_size += info.spans.size() * sizeof(android::ResStringPool_span) +
                    sizeof(android::ResStringPool_span::END);
    } else {
      auto info = m_styles16[i];
      encode_string16(info.string, info.len, &serialized_strings);
      spans_size += info.spans.size() * sizeof(android::ResStringPool_span) +
                    sizeof(android::ResStringPool_span::END);
    }
  }
  if (spans_size > 0) {
    spans_size += 2 * sizeof(android::ResStringPool_span::END);
  }
  // Rest of the strings
  for (size_t i = 0; i < non_style_string_count(); i++) {
    string_idx.push_back(serialized_strings.size());
    if (utf8) {
      auto pair = m_strings8[i];
      encode_string8(pair.key, pair.value, &serialized_strings);
    } else {
      auto pair = m_strings16[i];
      encode_string16(pair.key, pair.value, &serialized_strings);
    }
  }
  align_vec(4, &serialized_strings);
  auto string_data_size = serialized_strings.size() * sizeof(char);
  // ResChunk_header
  auto header_size = sizeof(android::ResStringPool_header);
  push_short(android::RES_STRING_POOL_TYPE, out);
  push_short(header_size, out);
  // Sum of header size, plus the size of all the string/style data.
  auto offsets_size = (string_idx.size() + span_off.size()) * sizeof(uint32_t);
  auto total_size = header_size + offsets_size + string_data_size + spans_size;
  push_long(total_size, out);
  // ResStringPool_header
  auto num_strings = string_count();
  push_long(num_strings, out);
  push_long(num_styles, out);
  // Write the same flags as given. No validation, callers expected to know what
  // they're doing.
  push_long(m_flags, out);
  // Strings start
  auto strings_start = header_size + offsets_size;
  push_long(strings_start, out);
  // Styles start
  auto styles_start = num_styles > 0 ? strings_start + string_data_size : 0;
  push_long(styles_start, out);
  // Write the string data
  for (const uint32_t& i : string_idx) {
    push_long(i, out);
  }
  // Offsets for spans
  for (const uint32_t& i : span_off) {
    push_long(i, out);
  }
  out->appendVector(serialized_strings);
  // Append spans
  for (size_t i = 0; i < num_styles; i++) {
    auto spans = utf8 ? m_styles8[i].spans : m_styles16[i].spans;
    for (size_t j = 0; j < spans.size(); j++) {
      auto span = spans[j];
      // Any struct that is copied directly to output is assumed to be in device
      // order. Not swapping.
      push_data_no_swap(span, sizeof(android::ResStringPool_span), out);
    }
    push_long(android::ResStringPool_span::END, out);
  }
  if (num_styles > 0) {
    push_long(android::ResStringPool_span::END, out);
    push_long(android::ResStringPool_span::END, out);
  }
}

namespace {
void write_string_pool(
  std::pair<std::shared_ptr<ResStringPoolBuilder>, android::ResStringPool_header*>& pair,
  android::Vector<char>* out) {
  if (pair.first != nullptr) {
    pair.first->serialize(out);
  } else {
    push_chunk((android::ResChunk_header*)pair.second, out);
  }
}
} // namespace

void ResPackageBuilder::serialize(android::Vector<char>* out) {
  android::Vector<char> temp;
  // Type strings
  write_string_pool(m_type_strings, &temp);
  auto type_strings_size = temp.size();
  write_string_pool(m_key_strings, &temp);
  // Types
  for (auto pair : m_id_to_type) {
    auto type_info = pair.second;
    push_chunk((android::ResChunk_header*)type_info.spec, &temp);
    for (auto type : type_info.configs) {
      push_chunk((android::ResChunk_header*)type, &temp);
    }
  }
  // All other chunks
  for (auto header : m_unknown_chunks) {
    push_chunk(header, &temp);
  }
  // ResTable_package's ResChunk_header
  auto header_size = sizeof(android::ResTable_package);
  push_short(android::RES_TABLE_PACKAGE_TYPE, out);
  push_short(header_size, out);
  auto total_size = header_size + temp.size();
  push_long(total_size, out);
  // ResTable_package's other members
  push_long(m_id, out);
  // Package name, this array is always a fixed size.
  for (size_t i = 0; i < PACKAGE_NAME_ARR_LENGTH; i++) {
    push_short(m_package_name[i], out);
  }
  // Offset to type strings, which are immediately after this header.
  push_long(header_size, out);
  push_long(m_last_public_type, out);
  // Offset to key strings, which are after the type strings
  push_long(header_size + type_strings_size, out);
  push_long(m_last_public_key, out);
  push_long(m_type_id_offset, out);
  out->appendVector(temp);
}

void ResTableBuilder::serialize(android::Vector<char>* out) {
  auto initial_size = out->size();
  // ResTable_header
  auto header_size = sizeof(android::ResTable_header);
  push_short(android::RES_TABLE_TYPE, out);
  push_short(header_size, out);
  auto total_size_pos = out->size();
  push_long(FILL_IN_LATER, out);
  push_long((uint32_t)m_packages.size(), out);
  // Global strings
  write_string_pool(m_global_strings, out);
  // Packages
  for (auto& pair : m_packages) {
    if (pair.first != nullptr) {
      pair.first->serialize(out);
    } else {
      push_chunk((android::ResChunk_header*)pair.second, out);
    }
  }
  write_long_at_pos(total_size_pos, out->size() - initial_size, out);
}

} // namespace arsc
