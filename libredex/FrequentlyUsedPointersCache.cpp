/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FrequentlyUsedPointersCache.h"

#include "DexClass.h"

void FrequentlyUsedPointers::load() {
#define LOAD_FREQUENTLY_USED_TYPE(func_name, java_name) \
  m_type_##func_name = DexType::make_type(java_name);   \
  m_well_known_types.insert(m_type_##func_name);
#define FOR_EACH LOAD_FREQUENTLY_USED_TYPE
  WELL_KNOWN_TYPES
#undef FOR_EACH

#define LOAD_FREQUENTLY_USED_FIELD(func_name, java_name) \
  m_field_##func_name = DexField::make_field(java_name);
#define FOR_EACH LOAD_FREQUENTLY_USED_FIELD
  PRIMITIVE_PSEUDO_TYPE_FIELDS
#undef FOR_EACH
}
