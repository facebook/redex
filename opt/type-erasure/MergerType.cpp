/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergerType.h"

#include <sstream>
#include <string>

std::string MergerType::Shape::build_type_name(
    const std::string& prefix,
    const std::string& name,
    size_t count,
    const boost::optional<size_t>& dex_num,
    const boost::optional<size_t>& interdex_subgroup_idx,
    const boost::optional<size_t>& subgroup_idx) const {
  std::ostringstream ss;
  ss << "L" << prefix << name;
  if (dex_num != boost::none) {
    ss << dex_num.get() << "_";
  }
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
