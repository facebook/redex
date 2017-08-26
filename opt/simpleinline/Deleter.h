/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "Resolver.h"
#include <functional>
#include <set>
#include <vector>

/**
 * Attempt to delete all removable candidates if there are no reference to
 * the method and the method is not marked as "do not delete".
 * Walks all opcodes in scope to check if the method is called.
 * A resolver must be provided to map a method reference to a method definition.
 */
size_t delete_methods(
    std::vector<DexClass*>& scope,
    std::unordered_set<DexMethod*>& removable,
    std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolver);

/**
 * Attempt to delete all removable candidates if there are no reference to
 * the method and the method is not marked as "do not delete".
 * Walks all opcodes in scope to check if the method is called.
 * Uses the default resolver to map method references to method definitions.
 */
inline size_t delete_methods(
    std::vector<DexClass*>& scope,
    std::unordered_set<DexMethod*>& removable) {
  return delete_methods(scope, removable,
      [](DexMethodRef* method, MethodSearch search) {
        return resolve_method(method, search);
  });
}
