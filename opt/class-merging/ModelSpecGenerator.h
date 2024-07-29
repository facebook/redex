/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
struct ConfigFiles;
using DexStoresVector = std::vector<DexStore>;
using Scope = std::vector<DexClass*>;
using DexClasses = std::vector<DexClass*>;

namespace class_merging {

class Model;

struct ModelSpec;

/**
 * Find all possible mergeables and roots by scanning the type hierarchy.
 * - Only leaf classes: not interface, not abstract, and has no subclasses.
 * - No throwable classes. ClassMerging service doesn't analyze throw edges and
 *   merging throwable classes has an chance to change the control flow.
 * - Only anonymous classes now but will change in the future.
 */
void find_all_mergeables_and_roots(const TypeSystem& type_system,
                                   const Scope& scope,
                                   size_t global_min_count,
                                   PassManager& mgr,
                                   ModelSpec* merging_spec,
                                   bool skip_dynamically_dead = false);

/**
 * Construct a merging model that disregards dex boundaries and max
 * size per merger. It is used by the mergeability-aware version of
 * the InterDexReshuffle pass, which is run right before
 * IntraDexClassMerging. As a result, certain merging specs used are
 * set to match those used by the IntraDexClassMerging pass.
 */
class_merging::Model construct_global_model(
    DexClasses& scope,
    PassManager& mgr,
    ConfigFiles& conf,
    DexStoresVector& stores,
    const class_merging::ModelSpec& merging_spec,
    size_t global_min_count);

} // namespace class_merging
