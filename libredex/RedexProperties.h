/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace redex_properties {

struct PropertyInteraction {
  bool establishes;
  bool requires_;
  bool preserves;
  bool requires_finally;
  PropertyInteraction(bool establishes,
                      bool requires_,
                      bool preserves,
                      bool requires_finally)
      : establishes(establishes),
        requires_(requires_),
        preserves(preserves),
        requires_finally(requires_finally) {}
  // "destroys" when !establishes && !preserves;
  bool is_valid() const {
    if (requires_ && establishes && !preserves) {
      return false;
    }
    if (requires_finally && !establishes) {
      return false;
    }
    return true;
  }
};

namespace interactions {
inline const PropertyInteraction Destroys = // default
    PropertyInteraction(false, false, false, false);
inline const PropertyInteraction Preserves =
    PropertyInteraction(false, false, true, false);
inline const PropertyInteraction Requires =
    PropertyInteraction(false, true, false, false);
inline const PropertyInteraction Establishes =
    PropertyInteraction(true, false, false, false);
inline const PropertyInteraction RequiresAndEstablishes =
    PropertyInteraction(true, true, true, false);
inline const PropertyInteraction EstablishesAndRequiresFinally =
    PropertyInteraction(true, false, false, true);
} // namespace interactions

enum class Property {
#define REDEX_PROPS(name, _neg, _init, _final) name,
#include "RedexProperties.def"
#undef REDEX_PROPS
};

bool is_negative(Property property);
std::vector<Property> get_all_properties();

const char* get_name(Property property);
std::ostream& operator<<(std::ostream& os, const Property& property);

using PropertyInteractions = std::unordered_map<Property, PropertyInteraction>;

// Legacy naming scheme. May update references at some point.

namespace names {

#define REDEX_PROPS(name, _neg, _init, _final) \
  constexpr Property name = Property::name;
#include "RedexProperties.def"
#undef REDEX_PROPS

} // namespace names

namespace simple {

// Only use for plain analysis passes. Otherwise it may be better to be
// explicit.
inline PropertyInteractions preserves_all() {
  using namespace redex_properties::interactions;
  return {
#define REDEX_PROPS(name, _neg, _init, _final) {Property::name, Preserves},
#include "RedexProperties.def"
#undef REDEX_PROPS
  };
}

} // namespace simple

} // namespace redex_properties
