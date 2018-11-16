/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

#include <unordered_map>
#include <unordered_set>

using TypeRelationship =
    std::unordered_map<DexClass*, std::unordered_set<DexClass*>>;
using Scope = std::vector<DexClass*>;

void optimize(Scope& scope,
              TypeRelationship& candidates,
              std::vector<DexClass*>& untfs,
              std::unordered_set<DexClass*>& removed);
