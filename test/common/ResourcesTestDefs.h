/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexResources.h"

inline size_t count_strings(const resources::StringOrReferenceSet& set,
                            const std::string& value) {
  resources::StringOrReference target(value);
  return set.count(target);
}

inline size_t count_refs(const resources::StringOrReferenceSet& set,
                         const uint32_t& value) {
  resources::StringOrReference target(value);
  return set.count(target);
}

inline size_t count_for_key(
    const std::unordered_multimap<std::string, resources::StringOrReference>&
        map,
    const std::string& key) {
  size_t result{0};
  auto range = map.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    result++;
  }
  return result;
}

inline std::unordered_set<std::string> string_values_for_key(
    const std::unordered_multimap<std::string, resources::StringOrReference>&
        map,
    const std::string& key) {
  std::unordered_set<std::string> result;
  auto range = map.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    if (!it->second.is_reference()) {
      result.emplace(it->second.str);
    }
  }
  return result;
}

inline bool is_overlayable(const std::string& name,
                           ResourceTableFile* res_table) {
  auto id = res_table->name_to_ids[name][0];
  return res_table->get_overlayable_id_roots().count(id) > 0;
}

// Android resource attribute IDs
const uint32_t kBackgroundAttrId = 0x010100d4;
const uint32_t kTextColorAttrId = 0x01010098;
const uint32_t kWindowActionBar = 0x010102cd;
const uint32_t kColorPrimaryAttrId = 0x01010433;
const uint32_t kColorAccent = 0x01010435;
const uint32_t kWindowNoTitle = 0x01010056;
const uint32_t kBackgroundTint = 0x010100d5;
const uint32_t kDrawableStart = 0x010100f4;
const uint32_t kDrawableEnd = 0x010100f5;
const uint32_t kTextSize = 0x01010095;
const uint32_t kTextStyleAttrId = 0x01010097;
const uint32_t kTextSizeAttrId = 0x01010095;
const uint32_t kEnabledAttrId = 0x0101000e;
const uint32_t kFloatAttrId = 0x01010099;
const uint32_t kDimensionAttrId = 0x0101009a;
const uint32_t kFractionAttrId = 0x0101009b;
const uint32_t kPaddingAttrId = 0x01010101;
const uint32_t kTextColorHintAttrId = 0x0101009A;

const uint32_t kColorPurple = 0xFFAA00BB;
const uint32_t kColorTeal = 0xFF00BBAA;
const uint32_t kColorBlue = 0xFF123456;

namespace sample_app {
inline std::vector<std::string> EXPECTED_OVERLAYABLE_RESOURCES{
    "button_txt", "log_msg", "log_msg_again", "welcome", "yummy_orange"};
} // namespace sample_app
