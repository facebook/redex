/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "SingleImplDefs.h"

#include "DexClass.h"
#include <unordered_map>

template <typename Container, typename Value>
inline bool exists(const Container& c, const Value& v) {
  return c.find(v) != c.end();
}

/**
 * Get the concrete implementation of an interface or nullptr if the
 * interface is not a single implemented one.
 */
DexType* get_concrete_type(SingleImpls& single_impls, DexType* type);
