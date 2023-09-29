/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Util.h"
#include "androidfw/ResourceTypes.h"

// Data that is used to write many test cases against. Meant to be included from
// individual test cpp files that want to code against it.

PACKED(struct EntryAndValue {
  android::ResTable_entry entry{};
  android::Res_value value{};
  EntryAndValue(uint32_t key_string_idx, uint8_t data_type, uint32_t data) {
    entry.size = sizeof(android::ResTable_entry);
    entry.key.index = key_string_idx;
    value.size = sizeof(android::Res_value);
    value.dataType = data_type;
    value.data = data;
  }
});

// For testing simplicity, a map that has two items in it.
PACKED(struct MapEntryAndValues {
  android::ResTable_map_entry entry{};
  android::ResTable_map item0{};
  android::ResTable_map item1{};
  MapEntryAndValues(uint32_t key_string_idx, uint32_t parent_ident) {
    entry.size = sizeof(android::ResTable_map_entry);
    entry.count = 2;
    entry.flags = android::ResTable_entry::FLAG_COMPLEX;
    entry.key.index = key_string_idx;
    entry.parent.ident = parent_ident;
    item0.value.size = sizeof(android::Res_value);
    item1.value.size = sizeof(android::Res_value);
  }
});

// Sample data for building arsc test cases
extern EntryAndValue e0;
extern EntryAndValue e0_land;
extern EntryAndValue e1;
extern EntryAndValue e2;
extern EntryAndValue id_0;
extern EntryAndValue id_1;
extern EntryAndValue id_2;
extern MapEntryAndValues style;

// A package called "foo"
extern android::ResTable_package foo_package;
// Create a default ResTable_config
extern android::ResTable_config default_config;
// Create a landscape config
extern android::ResTable_config land_config;
// And a xxhdpi config
extern android::ResTable_config xxhdpi_config;
