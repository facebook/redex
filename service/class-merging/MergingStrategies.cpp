/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <limits>

#include "ClassHierarchy.h"

#include "MergingStrategies.h"
#include "NormalizeConstructor.h"

namespace class_merging {
namespace strategy {

std::vector<std::vector<const DexType*>> group_by_cls_count(
    const TypeSet& mergeable_types,
    size_t min_mergeables_count,
    const boost::optional<size_t>& opt_max_mergeables_count) {
  size_t max_mergeables_count = opt_max_mergeables_count
                                    ? *opt_max_mergeables_count
                                    : std::numeric_limits<size_t>::max();
  redex_assert(min_mergeables_count <= max_mergeables_count &&
               min_mergeables_count >= 2);

  size_t remaining_count = mergeable_types.size();
  std::vector<std::vector<const DexType*>> groups;

  auto it = mergeable_types.begin();
  for (; remaining_count >= max_mergeables_count;
       remaining_count -= max_mergeables_count) {
    auto next = std::next(it, max_mergeables_count);
    std::vector<const DexType*> curr_group(it, next);
    groups.emplace_back(std::move(curr_group));
    it = next;
  }
  if (remaining_count >= min_mergeables_count) {
    std::vector<const DexType*> curr_group(it, mergeable_types.end());
    groups.emplace_back(std::move(curr_group));
  }
  return groups;
}

} // namespace strategy
} // namespace class_merging
