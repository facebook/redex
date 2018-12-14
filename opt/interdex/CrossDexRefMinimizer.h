/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DexClass.h"
#include "MutablePriorityQueue.h"

namespace interdex {

using PrioritizedDexClasses = MutablePriorityQueue<DexClass*, uint64_t>;
struct CrossDexRefMinimizerStats {
  size_t classes{0};
  size_t resets{0};
  size_t reprioritizations{0};
};

// Helper class that maintains a set of dex classes with associated priorities
// based on the *ref needs of the class and the *refs already added
// to the current dex.
//
// The priority of each class is determined as follows.
// - The primary priority is given by the ratio of already applied *refs to
//   unapplied *refs. This ratio is slightly tweaked in favor of
//   singleton *refs. ("Applied" refs are those which have already been added
//   to the current dex. "Singleton" refs are those for which there is only
//   one, namely this, class left.)
// - If there is a tie, use the original ordering as a tie breaker
// TODO: Try some other variations.
//
// (All this isn't entirely accurate, as it doesn't account for the dynamic
// behavior of plugins.)
class CrossDexRefMinimizer {
  PrioritizedDexClasses m_prioritized_classes;
  std::unordered_set<void*> m_applied_refs;
  struct ClassInfo {
    uint32_t index;
    uint32_t applied_refs;
    uint32_t singleton_refs;
    std::vector<void*> refs;
    ClassInfo(uint32_t i) : index(i), applied_refs(0), singleton_refs(0) {}
    uint64_t get_priority() const;
  };
  std::unordered_map<DexClass*, ClassInfo> m_class_infos;
  std::unordered_map<void*, std::unordered_set<DexClass*>> m_ref_classes;
  CrossDexRefMinimizerStats m_stats;

  struct ClassInfoDelta {
    int32_t applied_refs{0};
    int32_t singleton_refs{0};
  };
  void reprioritize(
      const std::unordered_map<DexClass*, ClassInfoDelta>& affected_classes);

 public:
  void insert(DexClass* cls);
  bool empty() const;
  DexClass* front() const;
  // "Erasing" a class applies its refs, updating
  // the priorities of all remaining classes.
  // "Resetting" must happen when the previous dex was flushed and the given
  // class is in fact applied to a new dex.
  // This function returns the number of applied refs.
  void erase(DexClass* cls, bool emitted, bool reset);
  const CrossDexRefMinimizerStats stats() const { return m_stats; }
};

} // namespace interdex
