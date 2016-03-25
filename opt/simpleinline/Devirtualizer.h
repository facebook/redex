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

#include <vector>

/**
 * Given a scope find all virtual method that can be devirtualized.
 * That is, methods that have a unique definition in the vmethods across
 * an hierarchy. Basically all methods that are virtual because of visibility
 * (public, package and protected) and not because they need to be virtual.
 */
std::vector<DexMethod*> devirtualize(std::vector<DexClass*>& scope);
