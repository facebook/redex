/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "androidfw/ResourceTypes.h"
#include "utils/Serialize.h"

// Data that is used to write many test cases against. Meant to be included from
// individual test cpp files that want to code against it.

struct EntryAndValue {
  android::ResTable_entry entry{};
  android::Res_value value{};
  EntryAndValue(uint32_t key_string_idx, uint8_t data_type, uint32_t data) {
    entry.size = sizeof(android::ResTable_entry);
    entry.key.index = key_string_idx;
    value.size = sizeof(android::Res_value);
    value.dataType = data_type;
    value.data = data;
  }
};

// For testing simplicity, a map that has two items in it.
struct MapEntryAndTwoValues {
  android::ResTable_map_entry entry{};
  android::ResTable_map item0{};
  android::ResTable_map item1{};

  template <typename T>
  static bool extract_at(const android::Vector<char>& data,
                         size_t index,
                         T* result) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Not trivially copyable, can't safely memcpy");
    const size_t size = sizeof(T);
    const size_t offset = index * size;

    if (offset + size <= data.size()) {
      memcpy(result, data.array() + offset, size);
      return true;
    }
    return false;
  }

  explicit MapEntryAndTwoValues(arsc::ResComplexEntryBuilder builder) {
    android::Vector<char> complex_entry_data;
    builder.serialize(&complex_entry_data);

    const size_t entry_size = sizeof(android::ResTable_map_entry);
    const size_t map_size = sizeof(android::ResTable_map);

    if (entry_size <= complex_entry_data.size()) {
      memcpy(&entry,
             complex_entry_data.array(),
             sizeof(android::ResTable_map_entry));

      auto count = entry.count;
      if (count > 0 && entry_size + map_size <= complex_entry_data.size()) {
        memcpy(&item0, complex_entry_data.array() + entry_size, map_size);

        if (count > 1 &&
            entry_size + 2 * map_size <= complex_entry_data.size()) {
          memcpy(&item1,
                 complex_entry_data.array() + entry_size + map_size,
                 map_size);
        }
      }
    }
  }
};

// Sample data for building arsc test cases
extern EntryAndValue e0;
extern EntryAndValue e0_land;
extern EntryAndValue e1;
extern EntryAndValue e2;
extern EntryAndValue id_0;
extern EntryAndValue id_1;
extern EntryAndValue id_2;
extern MapEntryAndTwoValues style;

// A package called "foo"
extern android::ResTable_package foo_package;
// Create a default ResTable_config
extern android::ResTable_config default_config;
// Create a landscape config
extern android::ResTable_config land_config;
// And a xxhdpi config
extern android::ResTable_config xxhdpi_config;
