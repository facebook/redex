/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"

namespace method_merger {

struct Stats {
  uint32_t num_merged_static_methods = 0;
  uint32_t num_merged_direct_methods = 0;
  uint32_t num_merged_nonvirt_methods = 0;
};
/**
 * The MethodGroups can carry groups for method merging, each group should all
 * be static, direct instance or virtual methods.
 */
using MethodGroups = std::vector<std::vector<DexMethod*>>;

/**
 * Each method group in method_groups should have the same method
 * type(static/direct/virtual), and shouldn't be constructors. Merge methods
 * based on proto grouping, then update all the invocations in the scope.
 * Return the method mapping from old method reference to new.
 */
Stats merge_methods(const MethodGroups& method_groups, const DexClasses& scope);

} // namespace method_merger
