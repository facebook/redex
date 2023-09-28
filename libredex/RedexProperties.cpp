/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexProperties.h"

#include <ostream>

namespace redex_properties {

bool is_negative(Property property) {
  switch (property) {
#define REDEX_PROPS(name, neg, _init, _final) \
  case Property::name:                        \
    return neg;
#include "RedexProperties.def"
#undef REDEX_PROPS
  }
  return false;
}

std::vector<Property> get_all_properties() {
  return {
#define REDEX_PROPS(name, _neg, _init, _final) Property::name,
#include "RedexProperties.def"
#undef REDEX_PROPS
  };
}

const char* get_name(Property property) {
  switch (property) {
#define REDEX_PROPS(name, _neg, _init, _final) \
  case Property::name:                         \
    return #name;
#include "RedexProperties.def"
#undef REDEX_PROPS
  }
  return "";
}

std::ostream& operator<<(std::ostream& os, const Property& property) {
  os << get_name(property);
  return os;
}

} // namespace redex_properties
