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

namespace sample_app {
inline std::vector<std::string> EXPECTED_OVERLAYABLE_RESOURCES{
    "button_txt", "log_msg", "log_msg_again", "welcome", "yummy_orange"};
} // namespace sample_app
