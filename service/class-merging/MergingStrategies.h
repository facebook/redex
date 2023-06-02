/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "ClassHierarchy.h"
#include "Trace.h"

class DexType;
using ConstTypeVector = std::vector<const DexType*>;
using GroupWalkerFn = std::function<void(const ConstTypeVector&)>;

/**
 * We can have multiple merging strategies for classes that have the same shape
 * and same interdex-group.
 */

namespace class_merging {
namespace strategy {

enum Strategy {
  // Starts a new group when a configurable number of merged classes is exceeded
  BY_CLASS_COUNT = 0,

  // Starts a new group when merged (virtual) methods become large
  BY_CODE_SIZE = 1,

  // Aggregates classes by shared references, and starts a new group when the
  // combined number of references becomes large, or when merged (virtual)
  // methods become large
  BY_REFS = 2,
};

struct GroupStats {
  size_t cls_count{0};
  size_t ref_count{0};
  size_t estimated_code_size{0};
  std::map<size_t, size_t> refs_stats{};

  void count(size_t cls_ref, size_t code_size) {
    if (!traceEnabled(CLMG, 5)) {
      return;
    }
    cls_count++;
    ref_count += cls_ref;
    estimated_code_size += code_size;
    refs_stats[cls_ref]++;
  }

  void reset() {
    cls_count = 0;
    ref_count = 0;
    estimated_code_size = 0;
    refs_stats.clear();
  }
};

class MergingStrategy final {

 public:
  MergingStrategy(const Strategy strategy, const TypeSet& mergeable_types)
      : m_strategy(strategy), m_mergeable_types(mergeable_types) {}

  void apply_grouping(size_t min_mergeables_count,
                      const boost::optional<size_t>& max_mergeables_count,
                      const GroupWalkerFn& walker) {
    switch (m_strategy) {
    case BY_CLASS_COUNT:
      group_by_cls_count(m_mergeable_types, min_mergeables_count,
                         max_mergeables_count, walker);
      break;
    case BY_CODE_SIZE:
      group_by_code_size(m_mergeable_types, walker);
      break;
    case BY_REFS:
      group_by_refs(m_mergeable_types, walker);
      break;
    default:
      not_reached();
    }
  }

 private:
  const Strategy m_strategy;
  const TypeSet& m_mergeable_types;

  void group_by_cls_count(
      const TypeSet& mergeable_types,
      size_t min_mergeables_count,
      const boost::optional<size_t>& opt_max_mergeables_count,
      const GroupWalkerFn& walker);

  /**
   * Note it does only check the virtual methods code size on the classes and it
   * is not aware of how later optimizations would change the code.
   */
  void group_by_code_size(const TypeSet& mergeable_types,
                          const GroupWalkerFn& walker);

  void group_by_refs(const TypeSet& mergeable_types,
                     const GroupWalkerFn& walker);
};

} // namespace strategy
} // namespace class_merging
