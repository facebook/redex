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

#include <unordered_map>
#include <unordered_set>

using TypeRelationship =
    std::unordered_map<DexClass*, std::unordered_set<DexClass*>>;
using Scope = std::vector<DexClass*>;

void optimize(Scope& scope, TypeRelationship& candidates,
    std::vector<DexClass*>& untfs, std::unordered_set<DexClass*>& removed);
