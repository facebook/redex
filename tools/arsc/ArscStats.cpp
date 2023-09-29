/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ArscStats.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

#include "ApkResources.h"
#include "Debug.h"
#include "RedexResources.h"
#include "Trace.h"
#include "androidfw/ResourceTypes.h"
#include "utils/ByteOrder.h"
#include "utils/Errors.h"
#include "utils/Log.h"
#include "utils/Serialize.h"
#include "utils/Unicode.h"
#include "utils/Vector.h"
#include "utils/Visitor.h"

using namespace attribution;

namespace {
constexpr size_t OFFSET_SIZE = sizeof(uint32_t);

// Will be iterated over for output, other collections can be unordered.
using ResourceSizes = std::map<uint32_t, ResourceSize>;
using ResourceConfigs = std::unordered_map<uint32_t, std::vector<std::string>>;
using TypeNames = std::unordered_map<uint8_t, std::string>;
using StringUsages = std::vector<std::set<uint32_t>>;

void add_size(const char* audit_msg,
              uint32_t id,
              size_t amount,
              size_t usage_count,
              ResourceSizes* resource_sizes) {
  always_assert(usage_count > 0);
  auto& size_struct = resource_sizes->at(id);
  if (usage_count == 1) {
    TRACE(ARSC, 2, "%s: 0x%x adding private size %zu", audit_msg, id, amount);
    size_struct.private_size += amount;
  }
  auto size = (double)amount / usage_count;
  TRACE(ARSC, 2, "%s: 0x%x adding proportional size (%zu / %zu) = %f",
        audit_msg, id, amount, usage_count, size);
  size_struct.proportional_size += size;
}

void add_shared_size(const char* audit_msg,
                     uint32_t id,
                     size_t amount,
                     ResourceSizes* resource_sizes) {
  auto& size_struct = resource_sizes->at(id);
  TRACE(ARSC, 2, "%s: 0x%x adding shared size %zu", audit_msg, id, amount);
  size_struct.shared_size += amount;
}

void populate_string_usages(apk::TableEntryParser& parser,
                            StringUsages* global_usages,
                            StringUsages* key_usages,
                            StringUsages* type_usages) {
  auto handle_value = [&](const uint32_t& id, android::Res_value* value_ptr) {
    if (value_ptr->dataType == android::Res_value::TYPE_STRING) {
      auto string_idx = dtohl(value_ptr->data);
      global_usages->at(string_idx).emplace(id);
    }
  };
  for (const auto& id_to_entries : parser.m_res_id_to_entries) {
    auto id = id_to_entries.first;
    for (const auto& pair : id_to_entries.second) {
      if (arsc::is_empty(pair.second)) {
        continue;
      }
      auto entry = (android::ResTable_entry*)pair.second.getKey();
      auto value = arsc::get_value_data(pair.second);

      auto key_idx = dtohl(entry->key.index);
      key_usages->at(key_idx).emplace(id);
      auto flags = dtohs(entry->flags);

      uint8_t type_id = (id & TYPE_MASK_BIT) >> TYPE_INDEX_BIT_SHIFT;
      type_usages->at(type_id - 1).emplace(id);

      if ((flags & android::ResTable_entry::FLAG_COMPLEX) != 0) {
        auto complex_entry = (android::ResTable_map_entry*)entry;
        auto count = dtohl(complex_entry->count);
        auto complex_item = (android::ResTable_map*)value.getKey();
        for (size_t i = 0; i < count; i++, complex_item++) {
          handle_value(id, &complex_item->value);
        }
      } else {
        auto value_ptr = (android::Res_value*)value.getKey();
        handle_value(id, value_ptr);
      }
    }
  }
}

template <typename T>
size_t length_units(size_t length) {
  // see aosp
  // https://cs.android.com/android/platform/superproject/main/+/main:frameworks/base/tools/aapt2/StringPool.cpp;l=356;drc=1dbbd3bd6ed466d9c3f284caad7adb8ed0f827d3
  static_assert(std::is_integral<T>::value);
  constexpr size_t MASK = 1 << ((sizeof(T) * 8) - 1);
  constexpr size_t MAX_SIZE = MASK - 1;
  return length > MAX_SIZE ? 2 : 1;
}

// Actual implementation of the string counting, which allows for
// differentiating whether or not we are currently computing a styled string
// (which is not expected to have a span index that is a style string).
size_t compute_string_size_impl(const android::ResStringPool& pool,
                                uint32_t idx,
                                bool allow_styles) {
  always_assert_log(idx < pool.size(),
                    "idx out of range, got %u for a pool of size %zu",
                    idx,
                    pool.size());
  size_t result = OFFSET_SIZE + compute_string_character_size(pool, idx);
  if (idx < pool.styleCount()) {
    always_assert_log(allow_styles,
                      "Got style index %u while computing size of style", idx);
    // for the span start
    result += OFFSET_SIZE;
    auto span_ptr = pool.styleAt(idx);
    std::vector<android::ResStringPool_span*> vec;
    arsc::collect_spans((android::ResStringPool_span*)span_ptr, &vec);
    result += vec.size() * sizeof(android::ResStringPool_span);
    for (const auto span : vec) {
      result += compute_string_size_impl(pool, dtohl(span->name.index), false);
    }
    result += sizeof(android::ResStringPool_span::END);
  }
  return result;
}

// Return the size of the string pool data structure header, padding, and END
// section, plus the string size for any unused string entries.
size_t compute_overhead(const android::ResStringPool_header* header,
                        const android::ResStringPool& pool,
                        const StringUsages& usages) {
  size_t padding = count_padding(header, pool);
  TRACE(ARSC, 1, "pool padding: %zu bytes", padding);
  size_t overhead = dtohs(header->header.headerSize) + padding;
  if (pool.styleCount() > 0) {
    overhead += 2 * sizeof(android::ResStringPool_span::END);
  }
  for (uint32_t idx = 0; idx < pool.size(); idx++) {
    if (usages.at(idx).empty()) {
      overhead += compute_string_size(pool, idx);
    }
    if (traceEnabled(ARSC, 3)) {
      auto str = arsc::get_string_from_pool(pool, idx);
      auto len = compute_string_size(pool, idx);
      TRACE_NO_LINE(ARSC, 3, "%u: \"%s\", length = %zu bytes. ", idx,
                    str.c_str(), len);
      auto set = usages.at(idx);
      if (set.empty()) {
        TRACE(ARSC, 3, "No uses.");
      } else {
        bool first = true;
        TRACE_NO_LINE(ARSC, 3, "Used by { ");
        for (const auto& id : set) {
          if (!first) {
            TRACE_NO_LINE(ARSC, 3, ", ");
          }
          TRACE_NO_LINE(ARSC, 3, "0x%x", id);
          first = false;
        }
        TRACE(ARSC, 3, " }");
      }
    }
  }
  return overhead;
}

void initialize_resource_sizes(const apk::TableEntryParser& parser,
                               ResourceSizes* resource_sizes) {
  for (const auto& entry : parser.m_res_id_to_entries) {
    resource_sizes->emplace(entry.first, ResourceSize{});
  }
}

// Attributes the size of string data and offsets to resource ids. Caller
// chooses whether a string value used by many ids should be considered as
// shared data or not.
void tally_string_sizes(const char* audit_msg,
                        const android::ResStringPool& pool,
                        const StringUsages& usages,
                        bool count_as_sharable,
                        size_t overhead,
                        ResourceSizes* resource_sizes) {
  auto entry_audit_message = std::string(audit_msg) + " pool entry";
  auto overhead_audit_message = std::string(audit_msg) + " pool overhead";

  std::set<uint32_t> all_ids;
  for (uint32_t idx = 0; idx < pool.size(); idx++) {
    auto set = usages.at(idx);
    if (!set.empty()) {
      auto amount = compute_string_size(pool, idx);
      for (const auto id : set) {
        auto usage_count = set.size();
        add_size(entry_audit_message.c_str(), id, amount, usage_count,
                 resource_sizes);
        if (usage_count > 1 && count_as_sharable) {
          add_shared_size("string pool", id, amount, resource_sizes);
        }
        all_ids.emplace(id);
      }
    }
  }
  for (const auto id : all_ids) {
    add_size(overhead_audit_message.c_str(), id, overhead, all_ids.size(),
             resource_sizes);
  }
}

// Attributes the size of string data and offsets to resource ids. Any string
// value that has many ids pointed to them will get counted as shared data.
void tally_string_sizes(const char* audit_msg,
                        const android::ResStringPool& pool,
                        const StringUsages& usages,
                        size_t overhead,
                        ResourceSizes* resource_sizes) {
  tally_string_sizes(audit_msg, pool, usages, true, overhead, resource_sizes);
}

// Attributes the typeSpec structure and zero to many type structures to the
// resource ids which are responsible for them. This is the step at which the
// table's chunk size and package header will be distributed to all non-empty
// resource ids.
std::set<uint32_t> tally_type_and_entries(
    const android::ResTable_package* package,
    const android::ResTable_typeSpec* type_spec,
    const std::vector<android::ResTable_type*>& types,
    apk::TableEntryParser& parser,
    android::ResStringPool& key_strings,
    ResourceSizes* resource_sizes,
    ResourceConfigs* resource_configs,
    ResourceNames* resource_names) {
  // Reverse map of actual data to the potentially many entries that it may
  // represent. This is to take into consideration the "canonical_entries" Redex
  // config item and make sure to represent this as shared size in the many ids
  // which can be represented with a single part of the arsc file.
  std::map<android::ResTable_entry*, std::set<uint32_t>> data_to_ids;
  std::set<uint32_t> non_empty_res_ids;
  std::unordered_map<android::ResTable_type*, std::set<uint32_t>>
      type_to_non_empty_ids;

  auto package_id = dtohl(package->id);
  auto type_id = type_spec->id;
  auto entry_count = dtohl(type_spec->entryCount);
  always_assert_log(entry_count <= std::numeric_limits<std::uint16_t>::max(),
                    "entry count %u too large for type",
                    entry_count);

  uint32_t upper =
      (PACKAGE_MASK_BIT & (package_id << PACKAGE_INDEX_BIT_SHIFT)) |
      (TYPE_MASK_BIT & (type_id << TYPE_INDEX_BIT_SHIFT));
  // Note: this vector could be empty.
  for (const auto& type : types) {
    for (uint16_t i = 0; i < entry_count; i++) {
      uint32_t res_id = upper | i;
      if (resource_configs->count(res_id) == 0) {
        std::vector<std::string> empty_vec;
        resource_configs->emplace(res_id, empty_vec);
      }
      auto ev = parser.get_entry_for_config(res_id, &type->config);
      if (!arsc::is_empty(ev)) {
        non_empty_res_ids.emplace(res_id);
        type_to_non_empty_ids[type].emplace(res_id);
        auto entry = (android::ResTable_entry*)ev.getKey();
        // Store name of entry and name of its configs.
        if (resource_names->count(res_id) == 0) {
          auto entry_name =
              arsc::get_string_from_pool(key_strings, dtohl(entry->key.index));
          resource_names->emplace(res_id,
                                  entry_name.empty() ? "unknown" : entry_name);
        }
        auto config_name = std::string(type->config.toString().string());
        if (config_name.empty()) {
          resource_configs->at(res_id).emplace_back("default");
        } else {
          resource_configs->at(res_id).emplace_back(config_name);
        }
        // Keep track of if we've seen a redundant pointer before
        data_to_ids[entry].emplace(res_id);
      }
    }
  }

  // typeSpec overhead will be the size of the header itself, plus 4 bytes for
  // every completely dead entry
  size_t spec_overhead = dtohs(type_spec->header.headerSize) +
                         (entry_count - non_empty_res_ids.size()) * OFFSET_SIZE;
  for (const auto res_id : non_empty_res_ids) {
    add_size("ResTable_typeSpec flag", res_id, OFFSET_SIZE, 1, resource_sizes);
    add_size("ResTable_typeSpec overhead",
             res_id,
             spec_overhead,
             non_empty_res_ids.size(),
             resource_sizes);
  }

  // Last step, re-iterate over the resource ids in each type, and compute
  // overhead of the type
  for (const auto& type : types) {
    auto& this_non_empty_set = type_to_non_empty_ids.at(type);
    size_t type_overhead = dtohs(type->header.headerSize);
    if ((type->flags & android::ResTable_type::FLAG_SPARSE) == 0) {
      type_overhead += (entry_count - this_non_empty_set.size()) * OFFSET_SIZE;
    }
    for (uint16_t i = 0; i < entry_count; i++) {
      uint32_t res_id = upper | i;
      auto ev = parser.get_entry_for_config(res_id, &type->config);
      if (!arsc::is_empty(ev)) {
        add_size("ResTable_type offset", res_id, OFFSET_SIZE, 1,
                 resource_sizes);
        add_size("ResTable_type overhead", res_id, type_overhead,
                 this_non_empty_set.size(), resource_sizes);
        auto entry = (android::ResTable_entry*)ev.getKey();
        auto entry_value_size = ev.getValue();

        auto& shared_set = data_to_ids.at(entry);
        always_assert_log(!shared_set.empty(),
                          "Inconsistent entry pointers for res id 0x%x",
                          res_id);
        add_size("ResTable_type entry and value", res_id, entry_value_size,
                 shared_set.size(), resource_sizes);
        if (shared_set.size() != 1) {
          add_shared_size("ResTable_type entry and value", res_id,
                          entry_value_size, resource_sizes);
        }
      }
    }
  }
  return non_empty_res_ids;
}

// Flattens data structures into an easily consumable form for outputting to a
// table / csv / whatever.
std::vector<Result> flatten(const ResourceSizes& resource_sizes,
                            const ResourceConfigs& resource_configs,
                            const ResourceNames& resource_names,
                            const TypeNames& type_names) {
  std::vector<Result> results;
  for (const auto& pair : resource_sizes) {
    auto res_id = pair.first;
    uint8_t type_id = (res_id >> TYPE_INDEX_BIT_SHIFT) & 0xFF;
    const auto& type_name = type_names.at(type_id);
    std::string resource_name;
    auto search = resource_names.find(res_id);
    if (search != resource_names.end()) {
      resource_name = search->second;
    }
    Result r{res_id, type_name, resource_name, pair.second,
             resource_configs.at(res_id)};
    results.emplace_back(r);
  }
  return results;
}
} // namespace

namespace attribution {
// Returns the number of bytes used to encode the string's length, the string's
// characters and the null zero.
size_t compute_string_character_size(const android::ResStringPool& pool,
                                     uint32_t idx) {
  size_t len;
  if (pool.isUTF8()) {
    auto ptr = pool.string8At(idx, &len);
    if (ptr != nullptr) {
      // UTF-8 length of this string will be either 1 or two bytes preceeding
      // the string.
      auto utf8_units = length_units<char>(len);
      // UTF-16 length is also stored, same way as above (one or two bytes)
      // preceeding the encoded UTF-8 length.
      auto utf16_length = utf8_to_utf16_length((const uint8_t*)ptr, len);
      auto utf16_units = length_units<char>(utf16_length);
      return utf16_units + utf8_units + len + 1;
    }
  } else {
    auto ptr = pool.stringAt(idx, &len);
    if (ptr != nullptr) {
      // length, char data, plus null zero.
      return (length_units<char16_t>(len) + len + 1) * sizeof(uint16_t);
    }
  }
  TRACE(ARSC, 1, "BAD STRING INDEX %u", idx);
  return 0;
}

// Character data that makes up the string pool needs to be 4 byte aligned. This
// counts how many bytes of padding were added.
size_t count_padding(const android::ResStringPool_header* header,
                     const android::ResStringPool& pool) {
  auto strings_start = dtohl(header->stringsStart);
  if (pool.size() == 0 || strings_start == 0) {
    return 0;
  }
  auto style_start = dtohl(header->stylesStart);
  auto strings_end =
      style_start == 0 ? dtohl(header->header.size) : style_start;
  auto total_characters_size = strings_end - strings_start;
  always_assert_log(total_characters_size >= 0, "Invalid string pool header");

  size_t current_characters_size = 0;
  for (uint32_t idx = 0; idx < pool.size(); idx++) {
    current_characters_size += compute_string_character_size(pool, idx);
  }
  always_assert_log(total_characters_size >= current_characters_size,
                    "Miscount of character data");
  return total_characters_size - current_characters_size;
}

// Return the number of bytes needed to encode the offset to string data, the
// number of bytes needed to encode the string's length, the character data, the
// null zero, and optionally how much data is needed to encode the spans and
// their character data.
size_t compute_string_size(const android::ResStringPool& pool, uint32_t idx) {
  return compute_string_size_impl(pool, idx, true);
}

std::vector<Result> ArscStats::compute() {
  apk::TableEntryParser parser;
  auto chunk_header = (android::ResChunk_header*)m_data;
  auto success = parser.visit(chunk_header, m_file_len);
  always_assert_log(success, "Could not parse arsc file!");
  // Maybe some day lift the following restriction, but we have no test data to
  // exercise >1 package so assert for now.
  always_assert_log(parser.m_packages.size() == 1, "Expected only 1 package.");
  auto package_header = *parser.m_packages.begin();

  // Step 1: parse the string pools and build up a vector of idx -> vector of
  // resource ids that use it.
  android::ResStringPool global_strings;
  auto global_strings_header = parser.m_global_pool_header;
  auto global_strings_size = dtohl(global_strings_header->header.size);
  {
    auto status =
        global_strings.setTo(global_strings_header, global_strings_size, true);
    always_assert_log(status == android::NO_ERROR,
                      "Could not parse global strings");
  }

  android::ResStringPool key_strings;
  auto key_strings_header = parser.m_package_key_string_headers.begin()->second;
  auto key_strings_size = dtohl(key_strings_header->header.size);
  {
    auto status = key_strings.setTo(key_strings_header, key_strings_size, true);
    always_assert_log(status == android::NO_ERROR,
                      "Could not parse key strings");
  }

  android::ResStringPool type_strings;
  auto type_strings_header =
      parser.m_package_type_string_headers.begin()->second;
  auto type_strings_size = dtohl(type_strings_header->header.size);
  {
    auto status =
        type_strings.setTo(type_strings_header, type_strings_size, true);
    always_assert_log(status == android::NO_ERROR,
                      "Could not parse type strings");
  }

  std::set<uint32_t> empty_set;
  StringUsages global_usages(global_strings.size(), empty_set);
  StringUsages key_usages(key_strings.size(), empty_set);
  StringUsages type_usages(type_strings.size(), empty_set);
  populate_string_usages(parser, &global_usages, &key_usages, &type_usages);

  TRACE(ARSC, 1, "Global strings size: %u", global_strings_size);
  auto global_overhead =
      compute_overhead(global_strings_header, global_strings, global_usages);
  TRACE(ARSC, 1, "Global strings overhead: %zu\n******************************",
        global_overhead);

  TRACE(ARSC, 1, "Key strings size: %u", key_strings_size);
  auto key_overhead =
      compute_overhead(key_strings_header, key_strings, key_usages);
  TRACE(ARSC, 1, "Key strings overhead: %zu\n******************************",
        key_overhead);

  TRACE(ARSC, 1, "Type strings size: %u", type_strings_size);
  auto type_strings_overhead =
      compute_overhead(type_strings_header, type_strings, type_usages);
  TRACE(ARSC, 1, "Type strings overhead: %zu\n******************************",
        type_strings_overhead);

  // All the various maps to hold output data.
  ResourceSizes resource_sizes;
  ResourceConfigs resource_configs;
  // Copy this to a new map, as the resid to name map may not be given. Any id
  // not present in the map will be outputted as it appears in the arsc file.
  ResourceNames resid_to_name(m_given_resid_to_name);
  TypeNames type_names;

  initialize_resource_sizes(parser, &resource_sizes);
  tally_string_sizes("global", global_strings, global_usages, global_overhead,
                     &resource_sizes);
  tally_string_sizes("key", key_strings, key_usages, key_overhead,
                     &resource_sizes);
  tally_string_sizes("type", type_strings, type_usages,
                     false /* don't count as sharable */, type_strings_overhead,
                     &resource_sizes);

  always_assert_log(type_strings.size() <= std::numeric_limits<uint8_t>::max(),
                    "type strings too large");
  for (uint8_t t = 0; t < type_strings.size(); t++) {
    auto type_name = arsc::get_string_from_pool(type_strings, t);
    type_names.emplace(t + 1, type_name);
  }

  // Add up sizes for every typeSpec and its type(s).
  std::set<uint32_t> all_non_empty_res_ids;
  for (auto& type_info : parser.m_package_types.at(package_header)) {
    // NOTE: we need to gather globally, the non-empty resource ids so we can
    // distribute the table_overhead figure above.
    auto non_empty_res_ids = tally_type_and_entries(package_header,
                                                    type_info.spec,
                                                    type_info.configs,
                                                    parser,
                                                    key_strings,
                                                    &resource_sizes,
                                                    &resource_configs,
                                                    &resid_to_name);
    all_non_empty_res_ids.insert(non_empty_res_ids.begin(),
                                 non_empty_res_ids.end());
  }
  auto table_overhead = dtohs(chunk_header->headerSize) +
                        dtohs(package_header->header.headerSize);
  for (const auto& res_id : all_non_empty_res_ids) {
    add_size("table, package headers", res_id, table_overhead,
             all_non_empty_res_ids.size(), &resource_sizes);
  }

  return flatten(resource_sizes, resource_configs, resid_to_name, type_names);
}
} // namespace attribution
