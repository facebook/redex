/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "WellKnownTypes.h"

class DexType;
class DexFieldRef;
class DexMethod;

#define STORE_TYPE(func_name, _)         \
 private:                                \
  DexType* m_type_##func_name = nullptr; \
                                         \
 public:                                 \
  DexType* type_##func_name() const { return m_type_##func_name; }

#define STORE_FIELDREF(func_name, _)          \
 private:                                     \
  DexFieldRef* m_field_##func_name = nullptr; \
                                              \
 public:                                      \
  DexFieldRef* field_##func_name() const { return m_field_##func_name; }

#define STORE_METHOD(func_name, _)           \
 private:                                    \
  DexMethod* m_method_##func_name = nullptr; \
                                             \
 public:                                     \
  DexMethod* method_##func_name() const { return m_method_##func_name; }

// The class is designed to cache frequently used pointers while invalidate them
// when RedexContext lifetime is over.
class FrequentlyUsedPointers {
 public:
  void load();

#define FOR_EACH STORE_TYPE
  WELL_KNOWN_TYPES
#undef FOR_EACH

#define FOR_EACH STORE_FIELDREF
  PRIMITIVE_PSEUDO_TYPE_FIELDS
#undef FOR_EACH

#define FOR_EACH STORE_METHOD
  WELL_KNOWN_METHODS
#undef FOR_EACH

  std::unordered_set<const DexType*> m_well_known_types;
};

#undef STORE_TYPE
#undef STORE_FIELDREF
#undef STORE_METHOD
