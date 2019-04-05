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

// For each (remaining) class, we are tracking (the weight of) each of its
// *refs for which there are only 1, 2, 3, or 4 classes left that also have
// that very same *ref.
// This information flows into the priority computation, so that the next
// selected class tends to have many *refs for which there are only few
// other classes left that also have those *refs.
// Generally, a higher count increases the effectiveness of cross-dex-reference
// minimization, but also causes it to use more memory and run slower.
constexpr size_t INFREQUENT_REFS_COUNT = 6;

using PrioritizedDexClasses = MutablePriorityQueue<DexClass*, uint64_t>;
struct CrossDexRefMinimizerStats {
  size_t classes{0};
  size_t resets{0};
  size_t reprioritizations{0};
  std::vector<std::pair<DexClass*, uint64_t>> worst_classes;
};

struct CrossDexRefMinimizerConfig {
  size_t method_ref_weight;
  size_t field_ref_weight;
  size_t type_ref_weight;
  size_t string_ref_weight;
};

// Helper class that maintains a set of dex classes with associated priorities
// based on the *ref needs of the class and the *refs already added
// to the current dex.
//
// The priority of each class is determined as follows.
// - The primary priority is given by the ratio of already applied *refs to
//   unapplied *refs. This ratio is slightly tweaked in favor of
//   infrequent *refs. ("Applied" refs are those which have already
//   been added to the current dex. "Infrequent" refs are those for which there
//   is only one, or two, ... classes left that reference them.)
// - If there is a tie, use the original ordering as a tie breaker
// TODO: Try some other variations.
//
// (All this isn't entirely accurate, as it doesn't account for the dynamic
// behavior of plugins.)
//
// A note on weights:
// - Individual ref weights are small unsigned numbers, tracked as uint32_t;
//   in pratice, they should be around 100 --- a number large enough to be
//   meaningfully divided by INFREQUENT_REFS_COUNT, which is relevant in the
//   priority computation
// - Large aggregate ref weights are unsigned numbers, tracked as uint64_t,
//   to really make sure that we don't overflow when adding individual refs
// - Deltas are tracked as signed integers, as they might be negative
//
// So in general, for weights, unsigned vs. signed indicates intent
// (can the number be negative?), and the width of the types should be
// reasonably large to prevent overflows. However, we don't always check for
// overflows. In any case, all of this flows into a heuristic, so it wouldn't
// be the end of the world if an overflow ever happens.
class CrossDexRefMinimizer {
  PrioritizedDexClasses m_prioritized_classes;
  std::unordered_set<void*> m_applied_refs;
  struct ClassInfo {
    uint32_t index;
    // This array stores (the weights of) how many of the *refs of this class
    // have only one, two, ... classes left that reference them.
    std::array<uint32_t, INFREQUENT_REFS_COUNT> infrequent_refs_weight;
    std::vector<std::pair<void*, uint32_t>> refs;
    uint64_t refs_weight;
    uint64_t applied_refs_weight;
    ClassInfo(uint32_t i)
        : index(i),
          infrequent_refs_weight(),
          refs_weight(0),
          applied_refs_weight(0) {}
    uint64_t get_primary_priority_denominator() const;
    uint64_t get_priority() const;
  };
  std::unordered_map<DexClass*, ClassInfo> m_class_infos;
  uint32_t m_next_index{0};
  std::unordered_map<void*, std::unordered_set<DexClass*>> m_ref_classes;
  CrossDexRefMinimizerStats m_stats;
  const CrossDexRefMinimizerConfig m_config;

  struct ClassInfoDelta {
    std::array<int32_t, INFREQUENT_REFS_COUNT> infrequent_refs_weight{};
    int64_t applied_refs_weight{0};
  };

  void reprioritize(
      const std::unordered_map<DexClass*, ClassInfoDelta>& affected_classes);
  DexClass* worst(bool generated);

  std::unordered_map<void*, size_t> m_ref_counts;
  size_t m_max_ref_count{0};

  void gather_refs(DexClass* cls,
                   std::vector<DexMethodRef*>& method_refs,
                   std::vector<DexFieldRef*>& field_refs,
                   std::vector<DexType*>& types,
                   std::vector<DexString*>& strings);

 public:
  CrossDexRefMinimizer(const CrossDexRefMinimizerConfig& config)
      : m_config(config) {}
  // Gather frequency counts; must be called for relevant classes before
  // inserting them
  void sample(DexClass* cls);
  void insert(DexClass* cls, bool ignore_cls = false);
  bool empty() const;
  DexClass* front() const;
  // "Worst" in the sense of having the biggest (adjusted) unapplied refs
  // weight.
  DexClass* worst();
  // "Erasing" a class applies its refs, updating
  // the priorities of all remaining classes.
  // "Resetting" must happen when the previous dex was flushed and the given
  // class is in fact applied to a new dex.
  // This function returns the number of applied refs.
  void erase(DexClass* cls, bool emitted, bool reset);
  const CrossDexRefMinimizerConfig& get_config() const { return m_config; }
  const CrossDexRefMinimizerStats& stats() const { return m_stats; }
};

} // namespace interdex
