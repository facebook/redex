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
  return property == Property::NeedsEverythingPublic ||
         property == Property::NeedsInjectionIdLowering;
}

std::vector<Property> get_all_properties() {
  return {
      Property::NoInitClassInstructions,  Property::NoUnreachableInstructions,
      Property::DexLimitsObeyed,          Property::NeedsEverythingPublic,
      Property::NeedsInjectionIdLowering, Property::HasSourceBlocks,
      Property::NoSpuriousGetClassCalls,  Property::RenameClass,
      Property::UltralightCodePatterns,
  };
}

const char* get_name(Property property) {
  switch (property) {
  case Property::NoInitClassInstructions:
    return "NoInitClassInstructions";
  case Property::NoUnreachableInstructions:
    return "NoUnreachableInstructions";
  case Property::DexLimitsObeyed:
    return "DexLimitsObeyed";
  case Property::NeedsEverythingPublic:
    return "NeedsEverythingPublic";
  case Property::NeedsInjectionIdLowering:
    return "NeedsInjectionIdLowering";
  case Property::HasSourceBlocks:
    return "HasSourceBlocks";
  case Property::NoResolvablePureRefs:
    return "NoResolvablePureRefs";
  case Property::NoSpuriousGetClassCalls:
    return "NoSpuriousGetClassCalls";
  case Property::InitialRenameClass:
    return "InitialRenameClass";
  case Property::RenameClass:
    return "RenameClass";
  case Property::UltralightCodePatterns:
    return "UltralightCodePatterns";
  }
  return "";
}

std::ostream& operator<<(std::ostream& os, const Property& property) {
  os << get_name(property);
  return os;
}

} // namespace redex_properties
