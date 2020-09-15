/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergerType.h"

#include "Trace.h"
#include <sstream>
#include <string>

namespace {

/**
 * Extract a minimal but identifiable name tag from the given root type.
 * E.g., "Lcom/facebook/analytics/structuredlogger/base/TypedEventBase;" ->
 * "EBase"
 */
std::string get_root_type_name_tag(const DexType* root_type) {
  auto root_type_name = type::get_simple_name(root_type);
  std::ostringstream root_name_tag;
  std::string::reverse_iterator rit = root_type_name.rbegin();
  // Scan the string from back to front.
  // Take out the last word in the simple type name starting with a cap letter.
  // E.g., "Lcom/facebook/TypedEventBase;" -> "esaB".
  for (; rit != root_type_name.rend(); ++rit) {
    auto c = *rit;
    if (isupper(c)) {
      root_name_tag << c;
      break;
    } else {
      root_name_tag << c;
    }
  }
  // Keep scanning backwards. Find the first cap letter of the second to last
  // word if any.
  // E.g., "Lcom/facebook/TypedEventBase;" -> "esaBE".
  for (++rit; rit != root_type_name.rend(); ++rit) {
    auto c = *rit;
    if (isupper(c)) {
      root_name_tag << c;
      break;
    }
  }
  auto root_name_tag_str = root_name_tag.str();
  // Apparantly, since we were traversing in reverse, we need to reverse the
  // name tag string.
  // E.g., "esaBE" -> "EBase".
  std::reverse(root_name_tag_str.begin(), root_name_tag_str.end());
  TRACE(CLMG, 7, "  root_name_tag %s", root_name_tag_str.c_str());
  return root_name_tag_str;
}

} // namespace

namespace class_merging {

std::string MergerType::Shape::build_type_name(
    const std::string& prefix,
    const DexType* root_type,
    const std::string& name,
    size_t count,
    const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
    const boost::optional<InterdexSubgroupIdx>& subgroup_idx) const {
  auto root_name_tag = get_root_type_name_tag(root_type);
  std::ostringstream ss;
  ss << "L" << prefix << root_name_tag << name;
  ss << count << "S" << string_fields << reference_fields << bool_fields
     << int_fields << long_fields << double_fields << float_fields;

  if (interdex_subgroup_idx != boost::none) {
    ss << "_I" << interdex_subgroup_idx.get();
  }

  if (subgroup_idx != boost::none) {
    ss << "_" << subgroup_idx.get();
  }
  ss << ";";
  return ss.str();
}

std::string MergerType::Shape::to_string() const {
  std::ostringstream ss;
  ss << "(" << string_fields << "," << reference_fields << "," << bool_fields
     << "," << int_fields << "," << long_fields << "," << double_fields << ","
     << float_fields << ")";
  return ss.str();
}

} // namespace class_merging
