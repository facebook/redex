/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

class DexClass;
class DexType;
class PassManager;
class DexStore;
class TypeSystem;
using DexStoresVector = std::vector<DexStore>;
using Scope = std::vector<DexClass*>;

namespace class_merging {

struct ModelSpec;

/**
 * Find all possible mergeables and roots by scanning the type hierarchy.
 * - Only leaf classes: not interface, not abstract, and has no subclasses.
 * - No throwable classes. ClassMerging service doesn't analyze throw edges and
 *   merging throwable classes has an chance to change the control flow.
 */
void find_all_mergeables_and_roots(const TypeSystem& type_system,
                                   const Scope& scope,
                                   size_t global_min_count,
                                   ModelSpec* merging_spec);

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
