/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "MergerType.h"
#include "PassManager.h"

struct ConfigFiles;
class JsonWrapper;

namespace class_merging {

struct ApproximateStats {
  // Number of shapes being merged
  size_t m_shapes_merged{0};
  // Number of mergeable classes being approximated
  size_t m_mergeables{0};
  // Number of additional fields added for shape merging. This is part of the
  // overhead of approximate shape merging
  size_t m_fields_added{0};

  ApproximateStats& operator+=(const ApproximateStats& stats) {
    m_shapes_merged += stats.m_shapes_merged;
    m_mergeables += stats.m_mergeables;
    m_fields_added += stats.m_fields_added;
    return *this;
  }

  void update_redex_stats(const std::string& prefix, PassManager& mgr) const {
    if (m_shapes_merged == 0) {
      return;
    }
    mgr.incr_metric(prefix + "_approx_shapes_merged", m_shapes_merged);
    mgr.incr_metric(prefix + "_approx_mergeables", m_mergeables);
    mgr.incr_metric(prefix + "_approx_fields_added", m_fields_added);
  }
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
