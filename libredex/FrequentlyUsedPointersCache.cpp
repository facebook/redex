/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FrequentlyUsedPointersCache.h"

#include "DexClass.h"

#define DEFINE_FREQUENTLY_USED_TYPE(func_name, java_name) \
  DexType* FrequentlyUsedPointers::func_name() {          \
    if (!m_##func_name) {                                 \
      m_##func_name = DexType::make_type(java_name);      \
    }                                                     \
    return m_##func_name;                                 \
  }

#define FOR_EACH DEFINE_FREQUENTLY_USED_TYPE
WELL_KNOWN_TYPES
#undef FOR_EACH
