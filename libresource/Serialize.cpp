/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <boost/functional/hash.hpp>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>

#include "androidfw/TypeWrappers.h"
#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/Unicode.h"
#include "utils/Vector.h"
#include "utils/Visitor.h"

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
constexpr uint16_t FILL_IN_LATER_SHORT = 0xEEEE;

void write_long_at_pos(size_t index,
                       uint32_t data,
                       android::Vector<char>* vec) {
  auto swapped = htodl(data);
  vec->replaceAt((char)swapped, index);
  vec->replaceAt((char)(swapped >> 8), index + 1);
  vec->replaceAt((char)(swapped >> 16), index + 2);
  vec->replaceAt((char)(swapped >> 24), index + 3);
}

void write_short_at_pos(size_t index,
                        uint16_t data,
                        android::Vector<char>* vec) {
  auto swapped = htods(data);
  vec->replaceAt((char)swapped, index);
  vec->replaceAt((char)(swapped >> 8), index + 1);
}

void encode_string8(const char* string,
                    size_t len,
                    android::Vector<char>* vec) {
  // aapt2 writes both the utf16 length followed by utf8 length
  auto u16_len = utf8_to_utf16_length((const uint8_t*)string, len);
  push_u8_length(u16_len, vec);
  push_u8_length(len, vec);
  // Push each char
  size_t i = 0;
  for (auto c = (char*)string; i < len; c++, i++) {
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
  size_t i = 0;
  for (uint16_t* c = (uint16_t*)s; i < len; c++, i++) {
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

// Does not swap byte order, just copy data as-is
void push_header(android::ResChunk_header* header, android::Vector<char>* out) {
  push_data_no_swap(header, dtohl(header->headerSize), out);
}

// Does not swap byte order of header, just copy data and update the size.
void push_header_with_updated_size(android::ResChunk_header* header,
                                   uint32_t new_size,
                                   android::Vector<char>* out) {
  auto start_pos = out->size();
  push_header(header, out);
  auto bytes_written = out->size() - start_pos;
  LOG_ALWAYS_FATAL_IF(
      bytes_written < sizeof(android::ResChunk_header),
      "Expected at least %ld header bytes. Actual %ld.",
      sizeof(android::ResChunk_header), bytes_written);
  write_long_at_pos(start_pos + 2 * sizeof(uint16_t), new_size, out);
}

void push_vec(android::Vector<char>& vec, android::Vector<char>* out) {
  if (!vec.empty()) {
    out->appendVector(vec);
  }
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

bool is_default_config(android::ResTable_config* c) {
  android::ResTable_config default_config{};
  default_config.size = sizeof(android::ResTable_config);
  return are_configs_equivalent(&default_config, c);
}

ssize_t find_attribute_ordinal(
    android::ResXMLTree_node* node,
    android::ResXMLTree_attrExt* extension,
    android::ResXMLTree_attribute* new_attr,
    const size_t& attribute_id_count,
    const std::function<std::string(uint32_t)>& pool_lookup) {
  std::vector<android::ResXMLTree_attribute*> attributes;
  collect_attributes(extension, &attributes);
  if (attributes.empty()) {
    return 0;
  }
  // Attributes are sorted first by id, when available, or sorted
  // lexographically by string name when the attribute does not
  // have an id. This is modelled after the aapt2 logic.
  // https://cs.android.com/android/platform/superproject/+/android-13.0.0_r1:frameworks/base/tools/aapt2/format/binary/XmlFlattener.cpp;l=45
  auto less_than = [&](android::ResXMLTree_attribute* this_attr,
                       android::ResXMLTree_attribute* that_attr) {
    auto this_name = dtohl(this_attr->name.index);
    auto this_uri = dtohl(this_attr->ns.index);
    auto this_has_id = this_name < attribute_id_count;

    auto that_name = dtohl(that_attr->name.index);
    auto that_uri = dtohl(that_attr->ns.index);
    auto that_has_id = that_name < attribute_id_count;

    if (this_has_id != that_has_id) {
      return this_has_id;
    } else if (this_has_id) {
      // names are offsets into id array, which is sorted, just compare name
      // index.
      return this_name < that_name;
    } else {
      // Compare uri first, if equal go to actual string name. Honestly this
      // does not make much sense since it is unclear how an attribute can have
      // a namespace and not an id. Hmmmmmmmmm.
      auto this_uri_str =
          this_uri != NO_VALUE ? pool_lookup(this_uri) : std::string("");
      auto that_uri_str =
          that_uri != NO_VALUE ? pool_lookup(that_uri) : std::string("");
      auto diff = this_uri_str.compare(that_uri_str);
      if (diff < 0) {
        return true;
      }
      if (diff > 0) {
        return false;
      }
      auto this_str = pool_lookup(this_name);
      auto that_str = pool_lookup(that_name);
      return this_str < that_str;
    }
  };
  // Find the first element that is greater than or equal to the new attribute.
  auto it = std::lower_bound(attributes.begin(), attributes.end(), new_attr,
                             less_than);
  if (it == attributes.end()) {
    return attributes.size();
  } else {
    // Check if the item we found is actually equal; this should be unsupported.
    if (!less_than(new_attr, *it)) {
      return -1;
    }
    return it - attributes.begin();
  }
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

bool ResTableTypeBuilder::should_encode_offsets_as_sparse(
    const std::vector<uint32_t>& offsets, size_t entry_data_size) {
  if (!m_enable_sparse_encoding) {
    return false;
  }
  if (entry_data_size / 4 > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  size_t total_non_empty = 0;
  for (const auto& i : offsets) {
    if (i != android::ResTable_type::NO_ENTRY) {
      if (i % 4 != 0) {
        // this should probably be fatal
        return false;
      }
      total_non_empty++;
    }
  }
  // See
  // https://cs.android.com/android/platform/superproject/+/android-12.0.0_r1:frameworks/base/tools/aapt2/format/binary/TableFlattener.cpp;l=382
  return (100 * total_non_empty) / offsets.size() < 60;
}

void ResTableTypeBuilder::encode_offsets_as_sparse(
    std::vector<uint32_t>* offsets) {
  std::vector<uint32_t> copy;
  copy.insert(copy.begin(), offsets->begin(), offsets->end());
  offsets->clear();
  uint16_t entry_id = 0;
  for (const auto& i : copy) {
    if (i != android::ResTable_type::NO_ENTRY) {
      android::ResTable_sparseTypeEntry entry;
      entry.idx = htods(entry_id);
      entry.offset = htods(i / 4);
      offsets->emplace_back(entry.entry);
    }
    entry_id++;
  }
}

bool ResTableTypeProjector::serialize_type(android::ResTable_type* type,
                                           size_t last_non_deleted,
                                           android::Vector<char>* out) {
  if (dtohl(type->entryCount) == 0 || dtohs(type->entriesStart) == 0) {
    // Wonky input data, omit this config.
    ALOGD("Wonky config for type %d, dropping!", type->id);
    return false;
  }
  // Check if this config has all of its entries deleted. If a non-default
  // config has everything deleted, skip emitting data.
  {
    size_t num_non_deleted_non_empty_entries = 0;
    android::TypeVariant tv(type);
    uint16_t i = 0;
    for (auto it = tv.beginEntries(); it != tv.endEntries(); ++it, ++i) {
      auto entry_ptr = const_cast<android::ResTable_entry*>(*it);
      auto id = make_id(i);
      auto is_deleted = m_ids_to_remove.count(id) != 0;
      if (!is_deleted && entry_ptr != nullptr) {
        num_non_deleted_non_empty_entries++;
      }
    }
    if (num_non_deleted_non_empty_entries == 0) {
      // No meaningful values for this config, don't emit the struct.
      return false;
    }
  }
  // Write entry/value data by iterating the existing offset data again, and
  // copying all non-deleted data to the temp vec.
  android::Vector<char> temp;
  std::vector<uint32_t> offsets;
  CanonicalEntries canonical_entries;
  // iterate again, now that we know it's useful
  android::TypeVariant tv(type);
  uint16_t i = 0;
  for (auto it = tv.beginEntries(); it != tv.endEntries(); ++it, ++i) {
    auto entry_ptr = const_cast<android::ResTable_entry*>(*it);
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) == 0) {
      if (entry_ptr == nullptr) {
        offsets.push_back(htodl(android::ResTable_type::NO_ENTRY));
      } else {
        uint32_t total_size =
            compute_entry_value_length((android::ResTable_entry*)entry_ptr);
        if (!m_enable_canonical_entries) {
          offsets.push_back(temp.size());
          // Copy the entry/value
          push_data_no_swap(entry_ptr, total_size, &temp);
        } else {
          // Check if we have already emitted identical data.
          EntryValueData ev((uint8_t*)entry_ptr, total_size);
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
    } else if (m_nullify_removed && i <= last_non_deleted) {
      offsets.push_back(htodl(android::ResTable_type::NO_ENTRY));
    }
  }
  uint8_t type_flags{0};
  if (should_encode_offsets_as_sparse(offsets, temp.size())) {
    encode_offsets_as_sparse(&offsets);
    type_flags |= android::ResTable_type::FLAG_SPARSE;
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
  auto num_offsets = offsets.size();
  auto entries_start = type_header_size + num_offsets * sizeof(uint32_t);
  auto total_size = entries_start + temp.size();
  push_long(total_size, out);
  out->push_back(m_type);
  out->push_back(type_flags);
  out->push_back(0); // pad to 4 bytes
  out->push_back(0);
  push_long(num_offsets, out);
  push_long(entries_start, out);
  auto cp = &type->config;
  push_data_no_swap(cp, config_size, out);
  for (size_t i = 0; i < num_offsets; i++) {
    push_long(offsets[i], out);
  }
  push_vec(temp, out);
  return true;
}

void ResTableTypeProjector::serialize(android::Vector<char>* out) {
  // Basic validation of the inputs given.
  LOG_ALWAYS_FATAL_IF(m_configs.empty(), "No configs given for type %d",
                      m_type);
  // Check if all entries in this type have been marked for deletion. If so, no
  // data is emitted.
  auto original_entries = dtohl(m_spec->entryCount);
  size_t num_deletions = 0;
  size_t last_non_deleted = 0;
  for (size_t i = 0; i < original_entries; i++) {
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) != 0) {
      num_deletions++;
    } else {
      last_non_deleted = i;
    }
  }
  if (num_deletions == original_entries) {
    // Nothing to do here.
    return;
  }
  // Write the ResTable_typeSpec header
  auto entries = m_nullify_removed ? last_non_deleted + 1
                                   : original_entries - num_deletions;
  push_short(android::RES_TABLE_TYPE_SPEC_TYPE, out);
  auto header_size = sizeof(android::ResTable_typeSpec);
  push_short(header_size, out);
  auto total_size = header_size + sizeof(uint32_t) * entries;
  push_long(total_size, out);
  out->push_back(m_type);
  out->push_back(0);
  // Number of types (used to be a reserved field). Will be stamped in later.
  auto type_count_pos = out->size();
  push_short(FILL_IN_LATER_SHORT, out);
  push_long(entries, out);
  // Copy all existing spec flags for non-deleted entries
  for (uint16_t i = 0; i < original_entries; i++) {
    auto id = make_id(i);
    if (m_ids_to_remove.count(id) == 0) {
      push_long(dtohl(get_spec_flags(m_spec, i)), out);
    } else if (m_nullify_removed && i <= last_non_deleted) {
      push_long(dtohl(0), out);
    }
  }
  // Write all applicable ResTable_type structures (and their corresponding
  // entries/values).
  uint16_t type_count{0};
  for (size_t i = 0; i < m_configs.size(); i++) {
    if (serialize_type(m_configs.at(i), last_non_deleted, out)) {
      type_count++;
    }
  }
  write_short_at_pos(type_count_pos, type_count, out);
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
  // Number of types (used to be a reserved field). Will be stamped in later.
  auto type_count_pos = out->size();
  push_short(FILL_IN_LATER_SHORT, out);
  push_long(entries, out);
  // Write all given spec flags
  for (uint16_t i = 0; i < entries; i++) {
    push_long(dtohl(m_flags.at(i)), out);
  }
  // Write the N configs given and all their entries/values
  uint16_t type_count{0};
  for (auto& config : m_configs) {
    if (empty_configs.count(config) > 0) {
      continue;
    }
    type_count++;
    auto& data = m_data.at(config);
    // Compute offsets and entry/value data size.
    CanonicalEntries canonical_entries;
    android::Vector<char> entry_data;
    std::vector<uint32_t> offsets;
    uint32_t offset = 0;
    for (auto& ev : data) {
      if (is_empty(ev)) {
        offsets.emplace_back(android::ResTable_type::NO_ENTRY);
      } else if (!m_enable_canonical_entries) {
        offsets.emplace_back(offset);
        offset += ev.value;
        push_data_no_swap(ev.key, ev.value, &entry_data);
      } else {
        size_t hash;
        uint32_t prev_offset;
        if (canonical_entries.find(ev, &hash, &prev_offset)) {
          offsets.emplace_back(prev_offset);
        } else {
          canonical_entries.record(ev, hash, offset);
          offsets.emplace_back(offset);
          offset += ev.value;
          push_data_no_swap(ev.key, ev.value, &entry_data);
        }
      }
    }
    uint8_t type_flags{0};
    if (should_encode_offsets_as_sparse(offsets, entry_data.size())) {
      encode_offsets_as_sparse(&offsets);
      type_flags |= android::ResTable_type::FLAG_SPARSE;
    }
    // Write the type header
    push_short(android::RES_TABLE_TYPE_TYPE, out);
    auto config_size = dtohs(config->size);
    auto type_header_size =
        sizeof(android::ResChunk_header) + sizeof(uint32_t) * 3 + config_size;
    push_short(type_header_size, out);
    auto entries_start = type_header_size + offsets.size() * sizeof(uint32_t);
    auto total_size = entries_start + entry_data.size();
    push_long(total_size, out);
    out->push_back(m_type);
    out->push_back(type_flags);
    out->push_back(0); // pad to 4 bytes
    out->push_back(0);
    push_long(offsets.size(), out);
    push_long(entries_start, out);
    push_data_no_swap(config, config_size, out);
    // Actual offsets and data.
    for (const auto& i : offsets) {
      push_long(i, out);
    }
    push_vec(entry_data, out);
  }
  write_short_at_pos(type_count_pos, type_count, out);
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
  m_styles.emplace_back(StringHolder(s, len), std::move(spans));
}

void ResStringPoolBuilder::add_style(const char16_t* s,
                                     size_t len,
                                     SpanVector spans) {
  m_styles.emplace_back(StringHolder(s, len), std::move(spans));
}

void ResStringPoolBuilder::add_style(std::string s, SpanVector spans) {
  auto len = s.length();
  m_styles.emplace_back(StringHolder(std::move(s), len), std::move(spans));
}

std::string ResStringPoolBuilder::get_string(size_t idx) {
  arsc::StringHolder holder = m_strings.at(idx);
  if (holder.is_char_ptr()) {
    return std::string(std::get<const char*>(holder.data));
  } else if (holder.is_char16_ptr()) {
    auto ptr = std::get<const char16_t*>(holder.data);
    android::String16 s16(ptr, holder.length);
    android::String8 s8(s16);
    return std::string(s8.string());
  } else {
    LOG_ALWAYS_FATAL_IF(!holder.is_str(), "unknown variant");
    return std::get<std::string>(holder.data);
  }
}

namespace {
void write_string8(const StringHolder& holder, android::Vector<char>* out) {
  if (holder.is_char_ptr()) {
    encode_string8(std::get<const char*>(holder.data), holder.length, out);
  } else if (holder.is_char16_ptr()) {
    android::String8 s8(std::get<const char16_t*>(holder.data), holder.length);
    size_t len = s8.length();
    encode_string8(s8.string(), len, out);
  } else {
    LOG_ALWAYS_FATAL_IF(!holder.is_str(), "unknown variant");
    android::String8 s8(std::get<std::string>(holder.data).c_str());
    size_t len = s8.length();
    encode_string8(s8.string(), len, out);
  }
}

void write_string16(const StringHolder& holder, android::Vector<char>* out) {
  if (holder.is_char_ptr()) {
    android::String8 s8(std::get<const char*>(holder.data), holder.length);
    android::String16 s16(s8);
    size_t len = s16.size();
    encode_string16(s16.string(), len, out);
  } else if (holder.is_char16_ptr()) {
    encode_string16(std::get<const char16_t*>(holder.data), holder.length, out);
  } else {
    LOG_ALWAYS_FATAL_IF(!holder.is_str(), "unknown variant");
    android::String16 s16(std::get<std::string>(holder.data).c_str());
    size_t len = s16.size();
    encode_string16(s16.string(), len, out);
  }
}

void write_string(bool utf8,
                  const StringHolder& holder,
                  android::Vector<char>* out) {
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
  std::vector<uint32_t> string_idx;
  std::vector<uint32_t> span_off;
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
  push_vec(serialized_strings, out);
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

uint32_t OverlayInfo::compute_size(
    android::ResTable_overlayable_policy_header* policy) const {
  auto entry_count = dtohl(policy->entry_count);
  if (entry_count == 0) {
    // This will be skipped during serialization.
    return 0;
  }
  return dtohs(policy->header.headerSize) + sizeof(uint32_t) * entry_count;
}

uint32_t OverlayInfo::compute_size() const {
  uint32_t policies_size{0};
  for (auto&& [policy, ids_ptr] : policies) {
    policies_size += compute_size(policy);
  }
  if (policies_size == 0) {
    // This will be skipped during serialization.
    return 0;
  }
  return dtohs(header->header.headerSize) + policies_size;
}

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
  for (auto& overlayable : m_overlays) {
    if (overlayable.empty()) {
      continue;
    }
    auto overlayable_size = overlayable.compute_size();
    push_header_with_updated_size((android::ResChunk_header*)overlayable.header,
                                  overlayable_size, &temp);
    for (auto [policy, ids] : overlayable.policies) {
      auto count = dtohl(policy->entry_count);
      if (count > 0) {
        auto policy_size = overlayable.compute_size(policy);
        push_header_with_updated_size((android::ResChunk_header*)policy,
                                      policy_size, &temp);
        for (size_t i = 0; i < count; i++) {
          push_long(ids[i], &temp);
        }
      }
    }
  }
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
  push_vec(temp, out);
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

void ResXmlIdsBuilder::serialize(android::Vector<char>* out) {
  if (!std::is_sorted(m_ids.begin(), m_ids.end())) {
    LOG_ALWAYS_FATAL("XML attribute ids should be sorted!");
  }
  uint32_t total_size =
      sizeof(android::ResChunk_header) + m_ids.size() * sizeof(uint32_t);
  push_short(android::RES_XML_RESOURCE_MAP_TYPE, out);
  push_short(sizeof(android::ResChunk_header), out);
  push_long(total_size, out);
  for (const auto& id : m_ids) {
    push_long(id, out);
  }
}

void replace_xml_string_pool(android::ResChunk_header* header,
                             size_t len,
                             ResStringPoolBuilder& builder,
                             android::Vector<char>* out) {
  // Find boundaries for the relevant piece of the file.
  auto data = (char*)header;
  auto chunk_size = dtohs(header->headerSize);
  auto pool_ptr = (android::ResStringPool_header*)(data + chunk_size);
  ResFileManipulator manipulator(data, len);
  manipulator.delete_at(pool_ptr, dtohl(pool_ptr->header.size));
  manipulator.add_serializable_at(pool_ptr, builder);
  manipulator.serialize(out);
}

int ensure_string_in_xml_pool(const void* data,
                              const size_t len,
                              const std::string& new_string,
                              android::Vector<char>* out_data,
                              size_t* idx) {
  std::unordered_map<std::string, uint32_t> out_idx;
  auto ret =
      ensure_strings_in_xml_pool(data, len, {new_string}, out_data, &out_idx);
  if (ret == android::OK) {
    *idx = out_idx.at(new_string);
  }
  return ret;
}

int ensure_strings_in_xml_pool(
    const void* data,
    const size_t len,
    const std::set<std::string>& strings_to_add,
    android::Vector<char>* out_data,
    std::unordered_map<std::string, uint32_t>* string_to_idx) {
  LOG_ALWAYS_FATAL_IF(!string_to_idx->empty(),
                      "string_to_idx should start empty");
  int validation_result = validate_xml_string_pool(data, len);
  if (validation_result != android::OK) {
    return validation_result;
  }
  SimpleXmlParser parser;
  LOG_ALWAYS_FATAL_IF(!parser.visit((void*)data, len), "Invalid file");
  auto& pool = parser.global_strings();
  size_t pool_size = pool.size();
  // Check if there is already a non-attribute with the given value.
  for (size_t i = parser.attribute_count(); i < pool_size; i++) {
    if (is_valid_string_idx(pool, i)) {
      auto s = get_string_from_pool(pool, i);
      if (strings_to_add.count(s) > 0) {
        string_to_idx->emplace(s, i);
      }
    }
  }

  if (strings_to_add.size() == string_to_idx->size()) {
    // Everything was already present, just return and do no futher work.
    // Convention to leave out_data unchanged in this case.
    return android::OK;
  }

  // Add given strings to the end of a new pool.
  auto flags = pool.isUTF8()
                   ? htodl((uint32_t)android::ResStringPool_header::UTF8_FLAG)
                   : (uint32_t)0;
  arsc::ResStringPoolBuilder pool_builder(flags);
  for (size_t i = 0; i < pool_size; i++) {
    pool_builder.add_string(pool, i);
  }

  for (const auto& s : strings_to_add) {
    if (string_to_idx->count(s) == 0) {
      auto idx = pool_builder.string_count();
      pool_builder.add_string(s);
      string_to_idx->emplace(s, idx);
    }
  }
  // Serialize new string pool into out data.
  replace_xml_string_pool((android::ResChunk_header*)data, len, pool_builder,
                          out_data);
  return android::OK;
}

int ensure_attribute_in_xml_doc(const void* const_data,
                                const size_t len,
                                const std::string& attribute_name,
                                const uint32_t& attribute_id,
                                android::Vector<char>* out_data,
                                size_t* idx) {
  LOG_ALWAYS_FATAL_IF(!out_data->empty(), "Output vector should start empty!");
  if (attribute_id == 0) {
    return ensure_string_in_xml_pool(const_data, len, attribute_name, out_data,
                                     idx);
  }

  constexpr ssize_t NOT_INSERTED = -1;
  char* data = (char*)const_data;

  SimpleXmlParser parser;
  LOG_ALWAYS_FATAL_IF(!parser.visit(data, len), "Invalid file");

  auto& pool = parser.global_strings();
  ssize_t insert_idx = NOT_INSERTED;
  ResStringPoolBuilder pool_builder(
      pool.isUTF8() ? android::ResStringPool_header::UTF8_FLAG : 0);
  ResXmlIdsBuilder ids_builder;
  for (size_t i = 0; i < parser.attribute_count(); i++) {
    auto id = parser.get_attribute_id(i);
    auto str = arsc::get_string_from_pool(pool, i);
    if (attribute_id == id) {
      if (str != attribute_name) {
        ALOGE("ID 0x%x already has conflicting name %s", id, str.c_str());
        return android::ALREADY_EXISTS;
      }
      *idx = i;
      return android::OK;
    }
    if (insert_idx == NOT_INSERTED && id > attribute_id) {
      insert_idx = i;
      pool_builder.add_string(attribute_name);
      ids_builder.add_id(attribute_id);
    }
    pool_builder.add_string(pool, i);
    ids_builder.add_id(id);
  }
  if (insert_idx == NOT_INSERTED) {
    insert_idx = parser.attribute_count();
    pool_builder.add_string(attribute_name);
    ids_builder.add_id(attribute_id);
  }
  // Copy over non-attribute strings to the pool builder.
  for (size_t i = parser.attribute_count(); i < pool.size(); i++) {
    pool_builder.add_string(pool, i);
  }

  // Build up a new file with the pool and edited attribute ids.
  ResFileManipulator manipulator(data, len);
  auto pool_off = data + parser.string_pool_offset();
  auto existing_pool_size = parser.string_pool_data_size();
  manipulator.delete_at(pool_off, existing_pool_size);
  manipulator.add_serializable_at(pool_off, pool_builder);

  auto attributes_header_offset = parser.attributes_header_offset();
  auto attributes_data_size = parser.attributes_data_size();
  if (attributes_header_offset != boost::none &&
      attributes_data_size != boost::none) {
    auto attributes_off = data + *attributes_header_offset;
    manipulator.delete_at(attributes_off, *attributes_data_size);
  }

  manipulator.add_serializable_at(pool_off + existing_pool_size, ids_builder);
  manipulator.serialize(out_data);

  // out_data now holds an inconsistent view; remap all string refs to be
  // consistent with what was added to the pool.
  std::unordered_map<uint32_t, uint32_t> mapping;
  for (size_t i = insert_idx; i < pool.size(); i++) {
    mapping.emplace(i, i + 1);
  }
  XmlStringRefRemapper remapper(mapping);
  if (remapper.visit((void*)out_data->array(), out_data->size())) {
    *idx = insert_idx;
    return android::OK;
  }
  LOG_ALWAYS_FATAL("Error parsing/remapping built file");
}

bool is_empty(const EntryValueData& ev) {
  if (ev.getKey() == nullptr) {
    LOG_ALWAYS_FATAL_IF(ev.getValue() != 0, "Invalid pointer, length pair");
    return true;
  }
  return false;
}

PtrLen<uint8_t> get_value_data(const EntryValueData& ev) {
  if (is_empty(ev)) {
    return {nullptr, 0};
  }
  auto entry_and_value_len = ev.getValue();
  auto entry = (android::ResTable_entry*)ev.getKey();
  auto entry_size = dtohs(entry->size);
  LOG_ALWAYS_FATAL_IF(entry_size > entry_and_value_len,
                      "Malformed entry size at %p", entry);
  if (entry_size == entry_and_value_len) {
    return {nullptr, 0};
  }
  auto ptr = (uint8_t*)entry + entry_size;
  return {ptr, entry_and_value_len - entry_size};
}

void ResFileManipulator::serialize(android::Vector<char>* out) {
  auto vec_start = out->size();
  ssize_t final_size = m_length;
  for (const auto& [c, block] : m_additions) {
    final_size += block.size;
  }
  for (const auto& [c, size] : m_deletions) {
    final_size -= size;
  }
  LOG_ALWAYS_FATAL_IF(final_size < 0, "final size went negative");
  // Copy the original data, applying our edits along the way.
  char* current = m_data;
  size_t i = 0;
  auto emit = [&](const Block& block) {
    out->appendArray((const char*)block.buffer.get(), block.size);
  };
  auto advance = [&](size_t amount) {
    i += amount;
    current += amount;
  };
  while (i < m_length) {
    auto addition = m_additions.find(current);
    if (addition != m_additions.end()) {
      emit(addition->second);
    }
    auto deletion = m_deletions.find(current);
    if (deletion != m_deletions.end()) {
      advance(deletion->second);
      continue;
    }
    out->push_back(*current);
    advance(1);
  }
  // Lastly, check if there is a request to add at the very end of the file.
  auto addition = m_additions.find(m_data + m_length);
  if (addition != m_additions.end()) {
    emit(addition->second);
  }
  // Assert everything is good.
  auto actual_size = out->size() - vec_start;
  LOG_ALWAYS_FATAL_IF(
      actual_size != final_size,
      "did not write expected number of bytes; wrote %zu, expected %zu",
      actual_size, (size_t)final_size);
  // Fix up the file size, assuming our original data starts in a proper chunk.
  if (actual_size >= sizeof(android::ResChunk_header)) {
    write_long_at_pos(vec_start + sizeof(uint16_t) * 2, final_size, out);
  }
}
} // namespace arsc
