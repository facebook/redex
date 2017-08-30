/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <map>
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
std::unordered_set<std::string> get_xml_files(
    const std::string& directory);
std::unordered_set<uint32_t> get_xml_reference_attributes(
    const std::string& filename);

/**
 * Follows the reference links for a resource for all configurations.
 * Returns all the nodes visited, as well as all the string values seen.
 */
void walk_references_for_resource(
   uint32_t resID,
   std::unordered_set<uint32_t>& nodes_visited,
   std::unordered_set<std::string>& leaf_string_values,
   android::ResTable* table);

std::unordered_set<uint32_t> get_js_resources(
   const std::string& directory,
   std::map<std::string, std::vector<uint32_t>> name_to_ids);

std::unordered_set<uint32_t> get_resources_by_name_prefix(
   std::vector<std::string> prefixes,
   std::map<std::string, std::vector<uint32_t>> name_to_ids);
