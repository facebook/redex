/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace optimize_enums {

/**
 * Enum instances are always stored as static fields with ACC_ENUM access flag.
 * The flag makes it easy to distinguish them from other fields on the enum
 * class.
 */
DexAccessFlags enum_field_access();

/**
 * Enum synthetic $VALUES field's access flag.
 */
DexAccessFlags synth_access();

struct EnumAttr {
  const uint32_t ordinal;
  const DexString* name; // Builtin name for enums.
};

using AttrMap = std::unordered_map<const DexField*, EnumAttr>;
/*
 * Returns a mapping of enum field -> ordinal value if success,
 * otherwise, return an empty map.
 */
AttrMap analyze_enum_clinit(const DexClass* cls);

} // namespace optimize_enums
