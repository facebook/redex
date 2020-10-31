/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <vector>

#include <boost/optional.hpp>

class DexType;

/**
 * We can have multiple merging strategies for classes that have the same shape
 * and same interdex-group.
 */

namespace class_merging {
namespace strategy {

std::vector<std::vector<const DexType*>> group_by_cls_count(
    const TypeSet& mergeable_types,
    size_t min_mergeables_count,
    const boost::optional<size_t>& max_mergeables_count);

} // namespace strategy
} // namespace class_merging
