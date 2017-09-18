/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>
#include <unordered_set>

#include "androidfw/ResourceTypes.h"

std::string read_entire_file(const std::string& filename);
std::string get_string_attribute_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name);
bool has_raw_attribute_value(
    const android::ResXMLTree& parser,
    const android::String16& attribute_name,
    android::Res_value& out_value);
std::unordered_set<std::string> get_manifest_classes(
    const std::string& filename);
std::unordered_set<std::string> get_native_classes(
    const std::string& apk_directory);
std::unordered_set<std::string> get_layout_classes(
    const std::string& apk_directory);
