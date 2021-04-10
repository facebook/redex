/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <unordered_set>
#include <vector>

class DexClass;
class DexType;
class PassManager;
class DexStore;
using DexStoresVector = std::vector<DexStore>;

namespace class_merging {

struct ModelSpec;

/**
 * Analyze type hierarchy to find anonymous classes to merge.
 * Fill the merging_spec with roots and merging_targets.
 */
void discover_mergeable_anonymous_classes(
    const DexStoresVector& stores,
    const std::unordered_set<std::string>& allowed_packages,
    size_t min_implementors,
    ModelSpec* merging_spec,
    PassManager* mgr);

} // namespace class_merging
