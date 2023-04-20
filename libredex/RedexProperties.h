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
  bool establishes{false};
  bool requires_{false};
  bool preserves{false};
  bool requires_finally{false};
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

using PropertyName = std::string;
using PropertyInteractions =
    std::unordered_map<PropertyName, PropertyInteraction>;

namespace names {

inline const PropertyName NoInitClassInstructions("NoInitClassInstructions");
inline const PropertyName DexLimitsObeyed("DexLimitsObeyed");
// Stand-in for fixing up passes.
inline const PropertyName NeedsEverythingPublic("NeedsEverythingPublic");
inline const PropertyName HasSourceBlocks("HasSourceBlocks");
inline const PropertyName NoSpuriousGetClassCalls("NoSpuriousGetClassCalls");
inline const PropertyName RenameClass("RenameClass");

} // namespace names
} // namespace redex_properties
