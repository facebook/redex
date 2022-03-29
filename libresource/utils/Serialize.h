/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_ANDROID_SERIALIZE_H
#define _FB_ANDROID_SERIALIZE_H

#include <map>
#include <memory>
#include <vector>

#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Debug.h"
#include "utils/Log.h"
#include "utils/String16.h"
#include "utils/String8.h"
#include "utils/TypeHelpers.h"
#include "utils/Unicode.h"
#include "utils/Vector.h"

namespace arsc {

constexpr uint32_t PACKAGE_NAME_ARR_LENGTH = 128;

void align_vec(size_t s, android::Vector<char>* vec);
void push_short(uint16_t data, android::Vector<char>* vec);
void push_long(uint32_t data, android::Vector<char>* vec);
void push_u8_length(size_t len, android::Vector<char>* vec);
void encode_string8(const android::String8& s, android::Vector<char>* vec);
void encode_string16(const android::String16& s, android::Vector<char>* vec);

// Returns the size of the entry and the value data structure(s) that follow it.
size_t compute_entry_value_length(android::ResTable_entry* entry);
// Return in device order the flags for the entry in the type
uint32_t get_spec_flags(android::ResTable_typeSpec* spec, uint16_t entry_id);

using SpanVector = std::vector<android::ResStringPool_span*>;

template <typename T>
struct StyleInfo {
  T* string;
  size_t len;
  SpanVector spans;
};

template <typename T>
using PtrLen = android::key_value_pair_t<T*, size_t>;

class ResStringPoolBuilder {
 public:
  ResStringPoolBuilder(uint32_t flags) : m_flags(flags) {}
  void add_string(const char*, size_t);
  void add_string(const char16_t*, size_t);
  void add_style(const char*, size_t, SpanVector);
  void add_style(const char16_t*, size_t, SpanVector);
  void serialize(android::Vector<char>* out);

  size_t string_count() { return non_style_string_count() + style_count(); }

 private:
  uint32_t m_flags;

  bool is_utf8() {
    return (m_flags & android::ResStringPool_header::UTF8_FLAG) != 0;
  }

  size_t non_style_string_count() {
    return is_utf8() ? m_strings8.size() : m_strings16.size();
  }

  size_t style_count() {
    return is_utf8() ? m_styles8.size() : m_styles16.size();
  }

  android::Vector<PtrLen<char16_t>> m_strings16;
  android::Vector<PtrLen<char>> m_strings8;
  android::Vector<StyleInfo<char16_t>> m_styles16;
  android::Vector<StyleInfo<char>> m_styles8;
};


using EntryValueData = PtrLen<uint8_t>;
// Struct for defining an existing type and the collection of entries in all
// configs.
struct TypeInfo {
  android::ResTable_typeSpec* spec;
  std::vector<android::ResTable_type*> configs;
};

// Builder class for copying existing data to a new/modified package.
// Subsequent work, to make this more full featured could be to define a
// ResTypeBuilder class, and let this append either TypeInfo (to copy existing
// data) or a builder to overhaul a type or define a brand new type.
class ResPackageBuilder {
 public:
  ResPackageBuilder() : m_package_name{0} {}
  // Copies fields from the source package that will remain unchanged in the
  // output (i.e. id, package name, etc).
  ResPackageBuilder(android::ResTable_package* package);
  void set_id(uint32_t id) { m_id = id; };
  void set_last_public_type(uint32_t last_public_type) {
    m_last_public_type = last_public_type;
  }
  void set_last_public_key(uint32_t last_public_key) {
    m_last_public_key = last_public_key;
  }
  void set_type_id_offset(uint32_t type_id_offset) {
    m_type_id_offset = type_id_offset;
  }
  // Copy the package name from an existing struct (in device order)
  void copy_package_name(android::ResTable_package* package) {
    for (uint32_t i = 0; i < PACKAGE_NAME_ARR_LENGTH; i++) {
      m_package_name[i] = dtohs(package->name[i]);
    }
  }
  void add_type(TypeInfo& info) {
    m_id_to_type.emplace(info.spec->id, info);
  }
  void set_key_strings(std::shared_ptr<ResStringPoolBuilder> builder) {
    m_key_strings.first = builder;
  }
  void set_key_strings(android::ResStringPool_header* existing_data) {
    m_key_strings.second = existing_data;
  }
  void set_type_strings(std::shared_ptr<ResStringPoolBuilder> builder) {
    m_type_strings.first = builder;
  }
  void set_type_strings(android::ResStringPool_header* existing_data) {
    m_type_strings.second = existing_data;
  }
  void add_chunk(android::ResChunk_header* header) {
    m_unknown_chunks.emplace_back(header);
  }
  void serialize(android::Vector<char>* out);

 private:
  // Pairs here are meant to be used like a union, set only one of them (defined
  // as a pair simply to inspect which is set).
  std::pair<std::shared_ptr<ResStringPoolBuilder>, android::ResStringPool_header*>
      m_key_strings;
  std::pair<std::shared_ptr<ResStringPoolBuilder>, android::ResStringPool_header*>
      m_type_strings;
  std::map<uint8_t, TypeInfo> m_id_to_type;
  // Chunks to emit after all type info. Meant to represent any unparsed struct
  // like libraries, overlay, etc.
  std::vector<android::ResChunk_header*> m_unknown_chunks;
  uint32_t m_id = 0;
  uint32_t m_last_public_type = 0;
  uint32_t m_last_public_key = 0;
  uint32_t m_type_id_offset = 0;
  uint16_t m_package_name[PACKAGE_NAME_ARR_LENGTH];
};

// Builder for a resource table, with support for either bulk appending package
// data or defining a new package with builder APIs.
class ResTableBuilder {
 public:
  void set_global_strings(std::shared_ptr<ResStringPoolBuilder> builder) {
    m_global_strings.first = builder;
  }
  void set_global_strings(android::ResStringPool_header* existing_data) {
    m_global_strings.second = existing_data;
  }
  void add_package(std::shared_ptr<ResPackageBuilder> builder) {
    m_packages.emplace_back(std::make_pair(builder, nullptr));
  }
  void add_package(android::ResTable_package* existing_data) {
    m_packages.emplace_back(std::make_pair(nullptr, existing_data));
  }
  void serialize(android::Vector<char>* out);

 private:
  // Pairs here are meant to be used like a union, set only one of them (defined
  // as a pair simply to inspect which is set).
  std::pair<std::shared_ptr<ResStringPoolBuilder>, android::ResStringPool_header*>
      m_global_strings;
  std::vector<std::pair<std::shared_ptr<ResPackageBuilder>, android::ResTable_package*>>
      m_packages;
};
} // namespace arsc
#endif
