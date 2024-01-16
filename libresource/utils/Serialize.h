/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_ANDROID_SERIALIZE_H
#define _FB_ANDROID_SERIALIZE_H

#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// Used for things like offsets to denote no value.
constexpr uint32_t NO_VALUE = 0xFFFFFFFF;

constexpr uint32_t PACKAGE_NAME_ARR_LENGTH = 128;

void align_vec(size_t s, android::Vector<char>* vec);
void push_short(uint16_t data, android::Vector<char>* vec);
void push_long(uint32_t data, android::Vector<char>* vec);
void push_u8_length(size_t len, android::Vector<char>* vec);
void encode_string8(const android::String8& s, android::Vector<char>* vec);
void encode_string16(const android::String16& s, android::Vector<char>* vec);

// Write the data to the file; overwrite existing data. Asserts that is was
// successful.
inline void write_bytes_to_file(const android::Vector<char>& vector,
                                const std::string& filename) {
  std::ofstream ofs(filename,
                    std::ofstream::out | std::ofstream::trunc |
                        std::ofstream::binary);
  ofs.write(vector.array(), vector.size());
  ofs.close();
  LOG_ALWAYS_FATAL_IF(!ofs, "Unable to write to %s", filename.c_str());
}

// Returns the size of the entry and the value data structure(s) that follow it.
size_t compute_entry_value_length(android::ResTable_entry* entry);
// Return in device order the flags for the entry in the type
uint32_t get_spec_flags(android::ResTable_typeSpec* spec, uint16_t entry_id);
// Whether or not the two configs should be treated as equal (note: this is not
// simply a byte by byte compare).
bool are_configs_equivalent(android::ResTable_config* a,
                            android::ResTable_config* b);

// For a Res_value marked with FLAG_COMPLEX, return the value part.
float complex_value(uint32_t complex);
// For a Res_value marked with FLAG_COMPLEX, return the unit part.
uint32_t complex_unit(uint32_t complex, bool isFraction);

// Returns whether or not idx is a non null string.
inline bool is_valid_string_idx(const android::ResStringPool& pool,
                                size_t idx) {
  size_t u16_len;
  return pool.stringAt(idx, &u16_len) != nullptr;
}

// Converts the string at given index, if needed, to utf-8 and returns it as
// std::string for convenience.
inline std::string get_string_from_pool(const android::ResStringPool& pool,
                                        size_t idx) {
  size_t u16_len;
  auto wide_chars = pool.stringAt(idx, &u16_len);
  android::String16 s16(wide_chars, u16_len);
  android::String8 string8(s16);
  return std::string(string8.string());
}

// Given a node, return the zero based ordinal where "new_attr" would appear,
// or -1 if the attribute already exists. This takes a callback function that is
// capable of returning the string bytes for a given index to follow the sorting
// convention used by aapt2.
ssize_t find_attribute_ordinal(
    android::ResXMLTree_node* node,
    android::ResXMLTree_attrExt* extension,
    android::ResXMLTree_attribute* new_attr,
    const size_t& attribute_id_count,
    const std::function<std::string(uint32_t)>& pool_lookup);

enum StringKind { STD_STRING, STRING_8, STRING_16 };

struct StringHolder {
  StringKind kind;
  const char* string8;
  const char16_t* string16;
  const std::string str;
  size_t length;
  StringHolder(const char* s, size_t len)
      : kind(StringKind::STRING_8),
        string8(s),
        string16(nullptr),
        length(len) {}
  StringHolder(const char16_t* s, size_t len)
      : kind(StringKind::STRING_16),
        string8(nullptr),
        string16(s),
        length(len) {}
  StringHolder(std::string s, size_t len)
      : kind(StringKind::STD_STRING),
        string8(nullptr),
        string16(nullptr),
        str(std::move(s)),
        length(len) {}
};

using SpanVector = std::vector<android::ResStringPool_span*>;

struct StyleInfo {
  StringHolder str;
  SpanVector spans;
};

template <typename T>
using PtrLen = android::key_value_pair_t<T*, size_t>;

class ResStringPoolBuilder {
 public:
  ResStringPoolBuilder(uint32_t flags) : m_flags(flags) {}
  // Note: in all cases, callers must be encoding string data properly, per
  // https://source.android.com/devices/tech/dalvik/dex-format#mutf-8
  void add_string(std::string);
  void add_string(const char*, size_t);
  void add_string(const char16_t*, size_t);
  // Insert string data from the given pool at the given index to the builder.
  void add_string(const android::ResStringPool& string_pool, size_t idx) {
    size_t length;
    if (string_pool.isUTF8()) {
      auto s = string_pool.string8At(idx, &length);
      add_string(s, length);
    } else {
      auto s = string_pool.stringAt(idx, &length);
      add_string(s, length);
    }
  }
  void add_style(std::string, SpanVector);
  void add_style(const char*, size_t, SpanVector);
  void add_style(const char16_t*, size_t, SpanVector);
  void serialize(android::Vector<char>* out);

  size_t string_count() { return non_style_string_count() + style_count(); }

 private:
  uint32_t m_flags;

  bool is_utf8() {
    return (m_flags & android::ResStringPool_header::UTF8_FLAG) != 0;
  }

  size_t non_style_string_count() { return m_strings.size(); }

  size_t style_count() { return m_styles.size(); }

  std::vector<StringHolder> m_strings;
  std::vector<StyleInfo> m_styles;
};

// From the given pointer to XML data (and the size of the data), write to `out`
// an equivalent XML doc, but with a string pool specified by the builder.
void replace_xml_string_pool(android::ResChunk_header* data,
                             size_t len,
                             ResStringPoolBuilder& builder,
                             android::Vector<char>* out);

// Parse the given binary xml bytes, and augments the string pool (if needed) to
// ensure that the given string is present and usable as a string ref. Return
// value will indicate whether or not the file was parsed successfully, and if
// parsed, the index of the given string is supplied to the output param
// (whether or not the pool was modified).
int ensure_string_in_xml_pool(const void* data,
                              const size_t len,
                              const std::string& new_string,
                              android::Vector<char>* out_data,
                              size_t* idx);
// Like above, but takes an ordered set of strings and returns a map to their
// indices.
int ensure_strings_in_xml_pool(
    const void* data,
    const size_t len,
    const std::set<std::string>& strings_to_add,
    android::Vector<char>* out_data,
    std::unordered_map<std::string, uint32_t>* string_to_idx);

using EntryValueData = PtrLen<uint8_t>;
using EntryOffsetData = std::pair<EntryValueData, uint32_t>;

bool is_empty(const EntryValueData& ev);

// Return a pointer to the start of values beyond the entry struct at the given
// pointer. Length returned will indicate how many more bytes there are that
// constiture the values. Callers MUST always check the length, since it could
// be zero (thus making the pointer not meaningful).
PtrLen<uint8_t> get_value_data(const EntryValueData& ev);

// Helper to record identical entry/value data that has already been emitted for
// a certain type.
class CanonicalEntries {
 public:
  CanonicalEntries() {}
  // Returns true and sets the offset output parameter if identical data has
  // already been noted. Sets the hash output param regardless.
  bool find(const EntryValueData& data, size_t* out_hash, uint32_t* out_offset);
  void record(EntryValueData data, size_t hash, uint32_t offset);

 private:
  size_t hash(const EntryValueData& data);
  // Hash to pair of the entry/value bytes with the hash code, and the offset to
  // the serialized data.
  std::unordered_map<size_t, std::vector<EntryOffsetData>> m_canonical_entries;
};

inline bool any_sparse_types(
    const std::vector<android::ResTable_type*>& configs) {
  for (const auto& t : configs) {
    if ((t->flags & android::ResTable_type::FLAG_SPARSE) != 0) {
      return true;
    }
  }
  return false;
}

// Builder for serializing a ResTable_typeSpec structure with N ResTable_type
// structures (and entries). As with other Builder classes, this can be used two
// ways:
// 1) Create new type, entry data.
// 2) Project deletions over existing data structures.
class ResTableTypeBuilder {
 public:
  ResTableTypeBuilder(uint32_t package_id,
                      uint8_t type,
                      bool enable_canonical_entries,
                      bool enable_sparse_encoding)
      : m_package_id(package_id),
        m_type(type),
        m_enable_canonical_entries(enable_canonical_entries),
        m_enable_sparse_encoding(enable_sparse_encoding) {
    LOG_ALWAYS_FATAL_IF((package_id & 0xFFFFFF00) != 0,
                        "package_id expected to have low byte set; got 0x%x",
                        package_id);
  }
  virtual ~ResTableTypeBuilder() {}
  uint8_t get_type_id() { return m_type; }
  uint32_t make_id(size_t entry) {
    return (m_package_id << 24) | (m_type << 16) | (entry & 0xFFFF);
  }
  bool should_encode_offsets_as_sparse(const std::vector<uint32_t>& offsets,
                                       size_t entry_data_size);
  void encode_offsets_as_sparse(std::vector<uint32_t>* offsets);
  virtual void serialize(android::Vector<char>* out) = 0;

 protected:
  // The (unshifted) number of the package to which this type belongs.
  uint32_t m_package_id;
  // The non-zero ID of this type
  uint8_t m_type;
  // Whether or not to check for redundant entry/value data.
  bool m_enable_canonical_entries;
  // Allows the encoding of a ResTable_type to set FLAG_SPARSE and emit
  // ResTable_sparseTypeEntry style entry offsets, if deemed beneficial for size
  bool m_enable_sparse_encoding;
};

// Builder for projecting deletions over existing data ResTable_typeSpec and its
// corresponding ResTable_type structures (as well as entries/values.)
class ResTableTypeProjector : public ResTableTypeBuilder {
 public:
  ResTableTypeProjector(uint32_t package_id,
                        android::ResTable_typeSpec* spec,
                        std::vector<android::ResTable_type*> configs,
                        bool enable_canonical_entries = false)
      : ResTableTypeBuilder(package_id,
                            spec->id,
                            enable_canonical_entries,
                            any_sparse_types(configs)),
        m_spec(spec),
        m_configs(std::move(configs)) {}
  void remove_ids(std::unordered_set<uint32_t>& ids_to_remove,
                  bool nullify_removed) {
    m_ids_to_remove = ids_to_remove;
    m_nullify_removed = nullify_removed;
  }
  void serialize(android::Vector<char>* out) override;
  virtual ~ResTableTypeProjector() {}

 private:
  void serialize_type(android::ResTable_type*,
                      size_t,
                      android::Vector<char>* out);
  android::ResTable_typeSpec* m_spec;
  std::vector<android::ResTable_type*> m_configs;
  // This takes effect during file serialization
  std::unordered_set<uint32_t> m_ids_to_remove;
  bool m_nullify_removed{false};
};

// Builder for defining a new ResTable_typeSpec along with its ResTable_type
// structures, entries, values. In all cases, given data should be in device
// order.
class ResTableTypeDefiner : public ResTableTypeBuilder {
 public:
  ResTableTypeDefiner(uint32_t package_id,
                      uint8_t id,
                      std::vector<android::ResTable_config*> configs,
                      std::vector<uint32_t> flags,
                      bool enable_canonical_entries = false,
                      bool enable_sparse_encoding = false)
      : ResTableTypeBuilder(
            package_id, id, enable_canonical_entries, enable_sparse_encoding),
        m_configs(std::move(configs)),
        m_flags(std::move(flags)) {}
  // Adds a chunk of data representing an entry and value to the given config.
  void add(android::ResTable_config* config, EntryValueData data) {
    auto search = m_data.find(config);
    if (search == m_data.end()) {
      std::vector<EntryValueData> vec;
      m_data.emplace(config, std::move(vec));
    }
    auto& vec = m_data.at(config);
    vec.emplace_back(data);
  }
  // Convenience method to add empty entry/value to the given config.
  void add_empty(android::ResTable_config* config) {
    EntryValueData ev(nullptr, 0);
    add(config, ev);
  }
  void serialize(android::Vector<char>* out) override;
  virtual ~ResTableTypeDefiner() {}

 private:
  // NOTE: size of m_configs should match the size of m_data. Inner vectors of
  // m_data should all have the same size, and that size should be equal to
  // m_flag's size.
  std::unordered_map<android::ResTable_config*, std::vector<EntryValueData>>
      m_data;
  const std::vector<android::ResTable_config*> m_configs;
  const std::vector<uint32_t> m_flags;
};

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
  // Adds type info which will be emitted as-is to the serialized package.
  void add_type(TypeInfo& info) {
    auto pair = std::make_pair(nullptr, info);
    m_id_to_type.emplace(info.spec->id, std::move(pair));
  }
  // Delegate to the builder to emit data when serializing.
  void add_type(std::shared_ptr<ResTableTypeBuilder> builder) {
    TypeInfo empty;
    auto pair = std::make_pair(builder, std::move(empty));
    m_id_to_type.emplace(builder->get_type_id(), std::move(pair));
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
  std::pair<std::shared_ptr<ResStringPoolBuilder>,
            android::ResStringPool_header*>
      m_key_strings;
  std::pair<std::shared_ptr<ResStringPoolBuilder>,
            android::ResStringPool_header*>
      m_type_strings;
  std::map<uint8_t, std::pair<std::shared_ptr<ResTableTypeBuilder>, TypeInfo>>
      m_id_to_type;
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
  std::pair<std::shared_ptr<ResStringPoolBuilder>,
            android::ResStringPool_header*>
      m_global_strings;
  std::vector<
      std::pair<std::shared_ptr<ResPackageBuilder>, android::ResTable_package*>>
      m_packages;
};

// Helper to organize edits to a binary chunk of data that is assumed to start
// in a ResChunk_header. It takes a chunk of original data, and allows for
// noting edits at certain positions and applying them later. Some basic
// conventions:
// 1) The deletions/additions are not expected to be disjointed. As a result,
//    deleting a range of data will not apply an addition within it (you can
//    delete bytes at pos N and add bytes at pos N, just not add at N+1).
// 2) Resulting file size will be computed, but this assumes that all the
//    operations are sensible, not disjointed, and don't ask for any change that
//    is out of bounds.
class ResFileManipulator {
 public:
  struct Block {
    Block(size_t s) : buffer(std::unique_ptr<char[]>(new char[s]())), size(s) {}

    template <typename T>
    void write(const T& item) {
      auto t_size = sizeof(T);
      LOG_ALWAYS_FATAL_IF(t_size + written_bytes > size,
                          "Will not write beyond the allocated size %zu", size);
      char* dest = buffer.get() + written_bytes;
      memcpy(dest, &item, t_size);
      written_bytes += t_size;
    }

    std::unique_ptr<char[]> buffer;
    size_t size;
    size_t written_bytes{0};
  };

  ResFileManipulator(char* data, size_t length)
      : m_data(data), m_length(length) {}

  void delete_at(void* pos, size_t size) {
    m_deletions.emplace((char*)pos, size);
  }
  void add_at(void* pos, Block block) {
    m_additions.emplace((char*)pos, std::move(block));
  }
  template <typename T>
  void add_at(void* pos, const T& item) {
    Block block(sizeof(T));
    block.write(item);
    m_additions.emplace((char*)pos, std::move(block));
  }
  // Shorthand for deleting N bytes at the position and adding N different
  // bytes.
  template <typename T>
  void replace_at(void* pos, const T& item) {
    delete_at(pos, sizeof(T));
    add_at(pos, item);
  }

  // Build the final file to the given vector.
  void serialize(android::Vector<char>* out);

 private:
  // At a given position, how many bytes to delete.
  std::unordered_map<char*, size_t> m_deletions;
  // Data that will be written in the given position.
  std::unordered_map<char*, Block> m_additions;

  // The original file data
  char* m_data;
  size_t m_length;
};
} // namespace arsc
#endif
