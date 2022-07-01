/*
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

struct EnumConstant {
  int32_t ordinal;
  const DexString* name;
};

union EnumFieldValue {
  int64_t primitive_value;
  const DexString* string_value;
};
/**
 * Maps enum ordinals to values for a particular instance field.
 */
using EnumInstanceFieldValueMap = std::map<int64_t, EnumFieldValue>;
/**
 * Maps enum instance fields to their value map for a particular enum.
 */
using EnumInstanceFieldMap =
    std::unordered_map<DexFieldRef*, EnumInstanceFieldValueMap>;
/**
 * Maps enum fields to its ordinal and name.
 */
using EnumConstantsMap = std::unordered_map<const DexFieldRef*, EnumConstant>;

struct EnumAttributes {
  EnumConstantsMap m_constants_map;
  EnumInstanceFieldMap m_field_map;

  std::map<uint64_t, const DexString*> get_ordered_names() {
    std::map<uint64_t, const DexString*> names;
    for (const auto& pair : m_constants_map) {
      names[pair.second.ordinal] = pair.second.name;
    }
    return names;
  }
};

/**
 * Returns an EnumInstanceFieldMap and an EnumConstantsMap if success,
 * otherwise, return empty maps.
 */
EnumAttributes analyze_enum_clinit(const DexClass* cls);

} // namespace optimize_enums
