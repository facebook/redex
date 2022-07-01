/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MergerType.h"

struct ConfigFiles;
class JsonWrapper;

namespace class_merging {

struct ApproximateStats {
  // Number of shapes being merged
  size_t shapes_merged{0};
  // Number of mergeable classes being approximated
  size_t mergeables{0};
  // Number of additional fields added for shape merging. This is part of the
  // overhead of approximate shape merging
  size_t fields_added{0};
};

/**
 * Approximate shape merging algorithms.
 *
 * If one shape contains another shape, we can approximate the smaller shape
 * using the bigger one, at the expense of some redundant fields. Approximate
 * shape merging algorithms declared in this module tries to reduce the number
 * of shapes by approximating smaller shapes with some bigger shapes.
 */

void simple_greedy_approximation(const JsonWrapper& specs,
                                 MergerType::ShapeCollector& shapes,
                                 ApproximateStats& stats);

void max_mergeable_greedy(const JsonWrapper& specs,
                          const ConfigFiles& conf,
                          MergerType::ShapeCollector& shapes,
                          ApproximateStats& stats);

void max_shape_merged_greedy(const JsonWrapper& specs,
                             const ConfigFiles& conf,
                             MergerType::ShapeCollector& shapes,
                             ApproximateStats& stats);

} // namespace class_merging
