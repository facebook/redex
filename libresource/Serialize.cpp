/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/functional/hash.hpp>
#include <iostream>

#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
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
  out->appendArray((const char*)data, length);
}

// Does not swap byte order, just copy data as-is
void push_chunk(android::ResChunk_header* header, android::Vector<char>* out) {
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

size_t compute_entry_value_length(android::ResTable_entry* entry) {
  if (entry == nullptr) {
    return 0;
  }
  auto entry_size = dtohs(entry->size);
  bool entry_is_complex =
      (dtohs(entry->flags) & android::ResTable_entry::FLAG_COMPLEX) != 0;
  if (entry_is_complex) {
    auto map_entry_ptr = (android::ResTable_map_entry*)entry;
    return entry_size +
           dtohl(map_entry_ptr->count) * sizeof(android::ResTable_map);
  } else {
    auto value = (android::Res_value*)((uint8_t*)entry + entry_size);
    return entry_size + dtohs(value->size);
  }
}

uint32_t get_spec_flags(android::ResTable_typeSpec* spec, uint16_t entry_id) {
  uint32_t* spec_flags =
      (uint32_t*)((uint8_t*)spec + dtohs(spec->header.headerSize));
  return *(spec_flags + entry_id);
}

namespace {
bool are_configs_equivalent_compat(android::ResTable_config* a,
                                   android::ResTable_config* b) {
  auto config_size = sizeof(android::ResTable_config);

  android::ResTable_config config_a{};
  memcpy(&config_a, a, dtohl(a->size));
  config_a.size = htodl(config_size);

  android::ResTable_config config_b{};
  memcpy(&config_b, b, dtohl(b->size));
  config_b.size = htodl(config_size);

  return memcmp(&config_a, &config_b, config_size) == 0;
}
} // namespace

bool are_configs_equivalent(android::ResTable_config* a,
                            android::ResTable_config* b) {
  auto a_size = dtohl(a->size);
  auto b_size = dtohl(b->size);
  if (a_size == b_size) {
    return memcmp(a, b, a_size) == 0;
  } else if (a_size <= sizeof(android::ResTable_config) &&
             b_size <= sizeof(android::ResTable_config)) {
    // Support some outdated .arsc file snapshots, files generated by older
    // tools, etc.
    return are_configs_equivalent_compat(a, b);
  }
  // Can't deal with newer ResTable_config layouts that we don't know about.
  return false;
}

float complex_value(uint32_t complex) {
  const float MANTISSA_MULT =
      1.0f / (1 << android::Res_value::COMPLEX_MANTISSA_SHIFT);
  const float RADIX_MULTS[] = {
      1.0f * MANTISSA_MULT, 1.0f / (1 << 7) * MANTISSA_MULT,
      1.0f / (1 << 15) * MANTISSA_MULT, 1.0f / (1 << 23) * MANTISSA_MULT};

  float value =
      (complex & (android::Res_value::COMPLEX_MANTISSA_MASK
                  << android::Res_value::COMPLEX_MANTISSA_SHIFT)) *
      RADIX_MULTS[(complex >> android::Res_value::COMPLEX_RADIX_SHIFT) &
                  android::Res_value::COMPLEX_RADIX_MASK];
  return value;
}

uint32_t complex_unit(uint32_t complex, bool isFraction) {
  return (complex >> android::Res_value::COMPLEX_UNIT_SHIFT) &
         android::Res_value::COMPLEX_UNIT_MASK;
}

size_t CanonicalEntries::hash(const EntryValueData& data) {
  size_t seed = 0;
  uint8_t* ptr = data.getKey();
  for (size_t i = 0; i < data.value; i++, ptr++) {
    boost::hash_combine(seed, *ptr);
  }
  return seed;
}

bool CanonicalEntries::find(const EntryValueData& data,
                            size_t* out_hash,
                            uint32_t* out_offset) {
  auto h = hash(data);
  *out_hash = h;
  auto search = m_canonical_entries.find(h);
  if (search != m_canonical_entries.end()) {
    auto data_size = data.value;
    auto& vec = search->second;
    for (const auto& pair : vec) {
      auto emitted_data = pair.first;
      if (data_size == emitted_data.value &&
          memcmp(data.key, emitted_data.key, data_size) == 0) {
        *out_offset = pair.second;
        return true;
      }
    }
  }
  return false;
}

void CanonicalEntries::record(EntryValueData data,
                              size_t hash,
                              uint32_t offset) {
  auto search = m_canonical_entries.find(hash);
  if (search == m_canonical_entries.end()) {
    std::vector<EntryOffsetData> vec;
    search = m_canonical_entries.emplace(hash, std::move(vec)).first;
  }
  auto p = std::make_pair(std::move(data), offset);
  search->second.emplace_back(p);
}

void ResTableTypeProjector::serialize_type(android::ResTable_type* type,
                                           android::Vector<char>* out) {
  auto original_entries = dtohl(type->entryCount);
  auto original_entries_start = dtohs(type->entriesStart);
  if (original_entries == 0 || original_entries_start == 0) {
    // Wonky input data, omit this config.
    ALOGD("Wonky config for type %d, dropping!", type->id);
    return;
  }
  // Check if this config has all of its entries deleted. If a non-default
  // config has everything deleted, skip emitting data.
  {
    size_t num_non_deleted_entries = 0;
    size_t num_non_deleted_non_empty_entries = 0;
    uint32_t* entry_offsets =
        (uint32_t*)((uint8_t*)type + dtohs(type->header.headerSize));
    for (size_t i = 0; i < original_entries; i++) {
      auto id = make_id(i);
      auto is_deleted = m_ids_to_remove.count(id) != 0;
      uint32_t offset = dtohl(entry_offsets[i]);
      if (!is_deleted) {
        num_non_deleted_entries++;
      }
      if (!is_deleted && offset != android::ResTable_type::NO_ENTRY) {
        num_non_deleted_non_empty_entries++;
      }
    }
    if (num_non_deleted_non_empty_entries == 0) {
      // No meaningful values for this config, don't emit the struct.
      return;
    }
  }
  // Write entry/value data by iterating the existing offset data again, and
  // copying all non-deleted data to the temp vec.
  android::Vector<char> temp;
  android::Vector<uint32_t> offsets;
  CanonicalEntries canonical_entries;
  // Pointer to the first Res_entry
  uint32_t* entry_offsets =
      (uint32_t*)((uint8_t*)type + dtohs(type->header.headerSize));
  for (size_t i = 0; i < original_entries; i++) {
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) == 0) {
      uint32_t offset = dtohl(entry_offsets[i]);
      if (offset == android::ResTable_type::NO_ENTRY) {
        offsets.push_back(htodl(android::ResTable_type::NO_ENTRY));
      } else {
        auto entry_ptr = (uint8_t*)type + dtohs(type->entriesStart) + offset;
        uint32_t total_size =
            compute_entry_value_length((android::ResTable_entry*)entry_ptr);
        if (!m_enable_canonical_entries) {
          offsets.push_back(temp.size());
          // Copy the entry/value
          push_data_no_swap(entry_ptr, total_size, &temp);
        } else {
          // Check if we have already emitted identical data.
          EntryValueData ev(entry_ptr, total_size);
          size_t hash;
          uint32_t prev_offset;
          if (canonical_entries.find(ev, &hash, &prev_offset)) {
            // No need to copy identical data, just emit the previous offset
            // again.
            offsets.push_back(prev_offset);
          } else {
            uint32_t this_offset = temp.size();
            canonical_entries.record(ev, hash, this_offset);
            offsets.push_back(this_offset);
            // Copy the entry/value just like we'd do if canonical offsets were
            // not enabled.
            push_data_no_swap(entry_ptr, total_size, &temp);
          }
        }
      }
    }
  }
  // Header and actual data structure
  push_short(android::RES_TABLE_TYPE_TYPE, out);
  // Derive the header size from the input data (guard against inputs generated
  // by older tool versions). Following code should not rely on either
  // sizeof(android::ResTable_type) or sizeof(android::ResTable_config)
  auto config_size = dtohs(type->config.size);
  auto type_header_size =
      sizeof(android::ResChunk_header) + sizeof(uint32_t) * 3 + config_size;
  push_short(type_header_size, out);
  auto num_entries = offsets.size();
  auto entries_start = type_header_size + num_entries * sizeof(uint32_t);
  auto total_size = entries_start + temp.size();
  push_long(total_size, out);
  out->push_back(m_type);
  out->push_back(0); // pad to 4 bytes
  out->push_back(0);
  out->push_back(0);
  push_long(num_entries, out);
  push_long(entries_start, out);
  auto cp = &type->config;
  push_data_no_swap(cp, config_size, out);
  for (size_t i = 0; i < num_entries; i++) {
    push_long(offsets[i], out);
  }
  out->appendVector(temp);
}

void ResTableTypeProjector::serialize(android::Vector<char>* out) {
  // Basic validation of the inputs given.
  LOG_ALWAYS_FATAL_IF(
      m_configs.size() == 0, "No configs given for type %d", m_type);
  // Check if all entries in this type have been marked for deletion. If so, no
  // data is emitted.
  auto original_entries = dtohl(m_spec->entryCount);
  size_t num_deletions = 0;
  for (size_t i = 0; i < original_entries; i++) {
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) != 0) {
      num_deletions++;
    }
  }
  if (num_deletions == original_entries) {
    // Nothing to do here.
    return;
  }
  // Write the ResTable_typeSpec header
  auto entries = original_entries - num_deletions;
  push_short(android::RES_TABLE_TYPE_SPEC_TYPE, out);
  auto header_size = sizeof(android::ResTable_typeSpec);
  push_short(header_size, out);
  auto total_size = header_size + sizeof(uint32_t) * entries;
  push_long(total_size, out);
  out->push_back(m_type);
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  push_long(entries, out);
  // Copy all existing spec flags for non-deleted entries
  for (uint16_t i = 0; i < original_entries; i++) {
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) == 0) {
      push_long(dtohl(get_spec_flags(m_spec, i)), out);
    }
  }
  // Write all applicable ResTable_type structures (and their corresponding
  // entries/values).
  for (size_t i = 0; i < m_configs.size(); i++) {
    serialize_type(m_configs.at(i), out);
  }
}

void ResTableTypeDefiner::serialize(android::Vector<char>* out) {
  // Validation
  LOG_ALWAYS_FATAL_IF(m_configs.size() != m_data.size(),
                      "Entry data not supplied for all configs");
  auto entries = m_flags.size();
  // Check whether or not we need to emit any data.
  std::unordered_set<android::ResTable_config*> empty_configs;
  for (auto& config : m_configs) {
    auto& data = m_data.at(config);
    LOG_FATAL_IF(data.size() != entries,
                 "Wrong number of entries for config, expected %zu",
                 entries);
    bool is_empty = true;
    for (auto& pair : data) {
      if (pair.key != nullptr && pair.value > 0) {
        is_empty = false;
        break;
      }
    }
    if (is_empty) {
      empty_configs.emplace(config);
    }
  }
  if (empty_configs.size() == m_configs.size()) {
    return;
  }

  // Write the ResTable_typeSpec header
  push_short(android::RES_TABLE_TYPE_SPEC_TYPE, out);
  auto header_size = sizeof(android::ResTable_typeSpec);
  push_short(header_size, out);
  auto total_size = header_size + sizeof(uint32_t) * entries;
  push_long(total_size, out);
  out->push_back(m_type);
  out->push_back(0);
  out->push_back(0);
  out->push_back(0);
  push_long(entries, out);
  // Write all given spec flags
  for (uint16_t i = 0; i < entries; i++) {
    push_long(dtohl(m_flags.at(i)), out);
  }
  // Write the N configs given and all their entries/values
  for (auto& config : m_configs) {
    if (empty_configs.count(config) > 0) {
      continue;
    }
    auto& data = m_data.at(config);
    // Write the type header
    push_short(android::RES_TABLE_TYPE_TYPE, out);
    auto config_size = dtohs(config->size);
    auto type_header_size =
        sizeof(android::ResChunk_header) + sizeof(uint32_t) * 3 + config_size;
    push_short(type_header_size, out);
    auto entries_start = type_header_size + data.size() * sizeof(uint32_t);
    // Write the final size later
    auto total_size_pos = out->size();
    push_long(FILL_IN_LATER, out);
    out->push_back(m_type);
    out->push_back(0); // pad to 4 bytes
    out->push_back(0);
    out->push_back(0);
    push_long(data.size(), out);
    push_long(entries_start, out);
    push_data_no_swap(config, config_size, out);
    // Compute and write offsets.
    CanonicalEntries canonical_entries;
    android::Vector<char> entry_data;
    uint32_t offset = 0;
    for (auto& ev : data) {
      if (ev.key == nullptr && ev.value == 0) {
        push_long(dtohl(android::ResTable_type::NO_ENTRY), out);
      } else if (!m_enable_canonical_entries) {
        push_long(offset, out);
        offset += ev.value;
        push_data_no_swap(ev.key, ev.value, &entry_data);
      } else {
        size_t hash;
        uint32_t prev_offset;
        if (canonical_entries.find(ev, &hash, &prev_offset)) {
          push_long(prev_offset, out);
        } else {
          canonical_entries.record(ev, hash, offset);
          push_long(offset, out);
          offset += ev.value;
          push_data_no_swap(ev.key, ev.value, &entry_data);
        }
      }
    }
    // Actual data.
    out->appendVector(entry_data);
    write_long_at_pos(total_size_pos, entries_start + entry_data.size(), out);
  }
}

void ResStringPoolBuilder::add_string(const char* s, size_t len) {
  StringHolder holder(s, len);
  m_strings.emplace_back(std::move(holder));
}

void ResStringPoolBuilder::add_string(const char16_t* s, size_t len) {
  StringHolder holder(s, len);
  m_strings.emplace_back(std::move(holder));
}

void ResStringPoolBuilder::add_string(std::string s) {
  auto len = s.length();
  StringHolder holder(std::move(s), len);
  m_strings.emplace_back(std::move(holder));
}

void ResStringPoolBuilder::add_style(const char* s,
                                     size_t len,
                                     SpanVector spans) {
  StringHolder holder(s, len);
  StyleInfo info{.str = std::move(holder), .spans = std::move(spans)};
  m_styles.emplace_back(std::move(info));
}

void ResStringPoolBuilder::add_style(const char16_t* s,
                                     size_t len,
                                     SpanVector spans) {
  StringHolder holder(s, len);
  StyleInfo info{.str = std::move(holder), .spans = std::move(spans)};
  m_styles.emplace_back(std::move(info));
}

void ResStringPoolBuilder::add_style(std::string s, SpanVector spans) {
  auto len = s.length();
  StringHolder holder(std::move(s), len);
  StyleInfo info{.str = std::move(holder), .spans = std::move(spans)};
  m_styles.emplace_back(std::move(info));
}

namespace {
void write_string8(StringHolder& holder, android::Vector<char>* out) {
  if (holder.kind == StringKind::STRING_8) {
    encode_string8(holder.string8, holder.length, out);
  } else if (holder.kind == StringKind::STRING_16) {
    android::String8 s8(holder.string16, holder.length);
    size_t len = s8.length();
    encode_string8(s8.string(), len, out);
  } else if (holder.kind == StringKind::STD_STRING) {
    android::String8 s8(holder.str.c_str());
    size_t len = s8.length();
    encode_string8(s8.string(), len, out);
  }
}

void write_string16(StringHolder& holder, android::Vector<char>* out) {
  if (holder.kind == StringKind::STRING_8) {
    android::String8 s8(holder.string8, holder.length);
    android::String16 s16(s8);
    size_t len = s16.size();
    encode_string16(s16.string(), len, out);
  } else if (holder.kind == StringKind::STRING_16) {
    encode_string16(holder.string16, holder.length, out);
  } else if (holder.kind == StringKind::STD_STRING) {
    android::String16 s16(holder.str.c_str());
    size_t len = s16.size();
    encode_string16(s16.string(), len, out);
  }
}

void write_string(bool utf8, StringHolder& holder, android::Vector<char>* out) {
  if (utf8) {
    write_string8(holder, out);
  } else {
    write_string16(holder, out);
  }
}
} // namespace

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
  for (auto& info : m_styles) {
    string_idx.push_back(serialized_strings.size());
    span_off.push_back(spans_size);
    write_string(utf8, info.str, &serialized_strings);
    spans_size += info.spans.size() * sizeof(android::ResStringPool_span) +
                  sizeof(android::ResStringPool_span::END);
  }
  if (spans_size > 0) {
    spans_size += 2 * sizeof(android::ResStringPool_span::END);
  }
  // Rest of the strings
  for (auto& string_holder : m_strings) {
    string_idx.push_back(serialized_strings.size());
    write_string(utf8, string_holder, &serialized_strings);
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
  for (auto& info : m_styles) {
    auto& spans = info.spans;
    for (size_t i = 0; i < spans.size(); i++) {
      auto& span = spans[i];
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
void write_string_pool(std::pair<std::shared_ptr<ResStringPoolBuilder>,
                                 android::ResStringPool_header*>& pair,
                       android::Vector<char>* out) {
  if (pair.first != nullptr) {
    pair.first->serialize(out);
  } else {
    push_chunk((android::ResChunk_header*)pair.second, out);
  }
}
} // namespace

ResPackageBuilder::ResPackageBuilder(android::ResTable_package* package) {
  set_id(dtohl(package->id));
  copy_package_name(package);
  set_last_public_key(dtohl(package->lastPublicKey));
  set_last_public_type(dtohl(package->lastPublicType));
  set_type_id_offset(dtohl(package->typeIdOffset));
}

void ResPackageBuilder::serialize(android::Vector<char>* out) {
  android::Vector<char> temp;
  // Type strings
  write_string_pool(m_type_strings, &temp);
  auto type_strings_size = temp.size();
  write_string_pool(m_key_strings, &temp);
  // Types
  for (auto& entry : m_id_to_type) {
    auto pair = entry.second;
    if (pair.first != nullptr) {
      pair.first->serialize(&temp);
    } else {
      auto type_info = pair.second;
      push_chunk((android::ResChunk_header*)type_info.spec, &temp);
      for (auto type : type_info.configs) {
        push_chunk((android::ResChunk_header*)type, &temp);
      }
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

void replace_xml_string_pool(android::ResChunk_header* data,
                             size_t len,
                             ResStringPoolBuilder& builder,
                             android::Vector<char>* out) {
  // Find boundaries for the various pieces of the file.
  auto chunk_size = dtohs(data->headerSize);
  auto pool_ptr = (android::ResStringPool_header*)((char*)data + chunk_size);
  // Build the new file.
  auto initial_vec_size = out->size();
  arsc::push_short(android::RES_XML_TYPE, out);
  arsc::push_short(chunk_size, out);
  auto total_size_pos = out->size();
  arsc::push_long(FILL_IN_LATER, out);
  builder.serialize(out);
  // Straight copy of everything after the original data's string pool.
  auto start = chunk_size + dtohl(pool_ptr->header.size);
  auto remaining = len - start;
  void* start_ptr = ((char*)data) + start;
  push_data_no_swap(start_ptr, remaining, out);
  write_long_at_pos(total_size_pos, out->size() - initial_vec_size, out);
}
} // namespace arsc
