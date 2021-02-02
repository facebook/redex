/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
 * The resolver must be thread-safe.
 */
size_t delete_methods(
    std::vector<DexClass*>& scope,
    std::unordered_set<DexMethod*>& removable,
    ConcurrentSet<DexMethod*>& delayed_make_static,
    std::function<DexMethod*(DexMethodRef*, MethodSearch)> concurrent_resolver);
