/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigUtils.h"

#include "Trace.h"

namespace class_merging {

namespace utils {

DexType* get_type(const std::string& type_s) {
  auto type = DexType::get_type(type_s);
  if (type == nullptr) {
    TRACE(CLMG, 2, "[ClassMerging] Warning: No type found for target type %s",
          type_s.c_str());
  }
  return type;
}

std::vector<DexType*> get_types(const std::vector<std::string>& target_types) {
  std::vector<DexType*> types;
  for (const auto& type_s : target_types) {
    auto target_type = get_type(type_s);
    if (target_type == nullptr) continue;
    types.push_back(target_type);
  }
  return types;
}

void load_types_and_prefixes(const std::vector<std::string>& type_names,
                             std::unordered_set<const DexType*>& types,
                             std::unordered_set<std::string>& prefixes) {
  for (const auto& type_s : type_names) {
    auto target_type = get_type(type_s);
    if (target_type == nullptr) {
      prefixes.insert(type_s);
    } else {
      types.insert(target_type);
    }
  }
}

} // namespace utils

} // namespace class_merging
