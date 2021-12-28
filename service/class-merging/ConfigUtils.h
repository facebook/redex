/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace class_merging {

namespace utils {

DexType* get_type(const std::string& type_s);

std::vector<DexType*> get_types(const std::vector<std::string>& target_types);

void load_types_and_prefixes(const std::vector<std::string>& type_names,
                             std::unordered_set<const DexType*>& types,
                             std::unordered_set<std::string>& prefixes);

} // namespace utils

} // namespace class_merging
