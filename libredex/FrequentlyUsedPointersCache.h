/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "WellKnownTypes.h"

class DexType;

#define STORE_TYPE(func_name, _)    \
 private:                           \
  DexType* m_##func_name = nullptr; \
                                    \
 public:                            \
  DexType* func_name();

// The class is designed to cache frequently used pointers while invalidate them
// when RedexContext lifetime is over.
class FrequentlyUsedPointers {
#define FOR_EACH STORE_TYPE
  WELL_KNOWN_TYPES
#undef FOR_EACH
};

#undef STORE_TYPE
