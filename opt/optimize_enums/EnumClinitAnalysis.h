/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace optimize_enums {

struct EnumAttr {
  const uint32_t ordinal;
  const DexString* name{nullptr}; // Builtin name for enums.
};

/*
 * Returns a mapping of enum field -> ordinal value.
 */
std::unordered_map<const DexField*, EnumAttr> analyze_enum_clinit(
    const DexClass* cls);

} // namespace optimize_enums
