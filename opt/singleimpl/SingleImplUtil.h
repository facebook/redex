/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "SingleImplDefs.h"

#include <unordered_map>
#include "DexClass.h"

template <typename Container, typename Value>
inline bool exists(const Container& c, const Value& v) {
  return c.find(v) != c.end();
}

/**
 * Get the concrete implementation of an interface or nullptr if the
 * interface is not a single implemented one.
 */
DexType* get_concrete_type(SingleImpls& single_impls, DexType* type);
