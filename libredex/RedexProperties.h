/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

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

using PropertyName = std::string;
using PropertyInteractions =
    std::unordered_map<PropertyName, PropertyInteraction>;

namespace names {

inline const PropertyName NoInitClassInstructions("NoInitClassInstructions");
inline const PropertyName DexLimitsObeyed("DexLimitsObeyed");
// Stand-in for fixing up passes.
inline const PropertyName NeedsEverythingPublic("NeedsEverythingPublic");
inline const PropertyName NeedsInjectionIdLowering("NeedsInjectionIdLowering");
inline const PropertyName HasSourceBlocks("HasSourceBlocks");
inline const PropertyName NoSpuriousGetClassCalls("NoSpuriousGetClassCalls");
inline const PropertyName RenameClass("RenameClass");
inline const PropertyName UltralightCodePatterns("UltralightCodePatterns");

} // namespace names

} // namespace redex_properties
