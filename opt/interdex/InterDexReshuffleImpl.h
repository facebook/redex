/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "PassManager.h"
#include "StlUtil.h"
#include "Walkers.h"

namespace class_merging {

// Forward declaration.
class Model;

} // namespace class_merging

struct Refs {
  MethodRefs mrefs;
  FieldRefs frefs;
  TypeRefs trefs;
  TypeRefs itrefs;
  std::unordered_set<const DexString*> srefs;
};

using gain_t = int64_t;

struct ReshuffleConfig {
  size_t reserved_extra_frefs{0};
  size_t reserved_extra_trefs{0};
  size_t reserved_extra_mrefs{0};
  size_t extra_linear_alloc_limit{0};
  size_t max_batches{20};
  size_t max_batch_size{200000};
  size_t interaction_frequency_threshold{0};
  bool exclude_below20pct_coldstart_classes{false};
  // Class merging related
  gain_t deduped_weight{1};
  gain_t other_weight{1};
};

struct MergingInfo {
  MergerIndex merging_type;
  std::unordered_map<const DexMethod*, MethodGroup> dedupable_mrefs;
};

// Compute gain powers by reference occurrences. We don't use the upper 20 (19,
// actually, because sign bit) bits to allow for adding all gains of a class.
// TODO: While this integer-based representation allows for a fast and
// deterministic algorithm, it's precision ends at more than 11 occurrences,
// where the gain goes to 0. Based on current experiments, increasing 11 may
// increase the size gain a little bit, but come with a cost of
// “non-determinism” (due to rounding errors or space complexity if we compute
// these differently).
static constexpr gain_t power_value_for(size_t occurrences) {
  return occurrences > 11 ? 0 : (((gain_t)1) << 44) >> (occurrences * 4);
}

// A suggested move of a class from one dex to another.
struct Move {
  DexClass* cls;
  gain_t gain;
  size_t target_dex_index;
};

// All move gains for all classes.
class MoveGains {
  auto compare_indices_by_gains() {
    return [&](size_t first_gain_index, size_t second_gain_index) {
      auto& first = m_gains.at(first_gain_index);
      auto& second = m_gains.at(second_gain_index);
      if (first.gain != second.gain) {
        return first.gain < second.gain;
      }
      // tie breakers for determinism
      if (first.target_dex_index != second.target_dex_index) {
        return first.target_dex_index < second.target_dex_index;
      }
      return compare_dexclasses(first.cls, second.cls);
    };
  }

 public:
  MoveGains(size_t first_dex_index,
            const std::vector<DexClass*>& movable_classes,
            const std::unordered_map<DexClass*, size_t>& class_dex_indices,
            const std::unordered_map<DexClass*, Refs>& class_refs,
            const std::vector<DexStructure>& dexen,
            const std::vector<std::unordered_map<const DexString*, size_t>>&
                dexen_strings,
            const std::unordered_set<size_t>& dynamically_dead_dexes,
            const std::unordered_map<DexClass*, struct MergingInfo>&
                class_to_merging_info,
            const std::unordered_map<MergerIndex, size_t>& num_field_defs,
            const bool mergeability_aware,
            const gain_t deduped_weight,
            const gain_t other_weight)
      : m_first_dex_index(first_dex_index),
        m_movable_classes(movable_classes),
        m_class_dex_indices(class_dex_indices),
        m_class_refs(class_refs),
        m_dexen(dexen),
        m_dexen_strings(dexen_strings),
        m_dynamically_dead_dexes(dynamically_dead_dexes),
        m_class_to_merging_info(class_to_merging_info),
        m_num_field_defs(num_field_defs),
        m_mergeability_aware(mergeability_aware),
        m_deduped_weight(deduped_weight),
        m_other_weight(other_weight) {}

  void recompute_gains(size_t removal_dex = 0) {
    Timer t("recompute_gains");
    m_gains_size = 0;
    std::mutex mutex;
    walk::parallel::classes(m_movable_classes, [&](DexClass* cls) {
      for (size_t dex_index = m_first_dex_index; dex_index < m_dexen.size();
           ++dex_index) {
        if (m_dynamically_dead_dexes.count(dex_index) != 0) {
          // m_dynamically_dead_dexes should not be involved during reshuffle.
          continue;
        }
        if (!m_moved_classes.empty() && m_moved_classes.count(cls)) {
          // In DexRemovalPass, if a class is already moved from the dex which
          // is going to be eliminated, we won't move it again.
          continue;
        }

        if (dex_index == removal_dex) {
          // Won't move any class to the potential removed dex.
          continue;
        }
        gain_t gain = 0;
        if (m_mergeability_aware) {
          gain = compute_move_gain_after_merging(
              cls, dex_index, removal_dex != 0 ? true : false);
        } else {
          gain = compute_move_gain(cls, dex_index,
                                   removal_dex != 0 ? true : false);
        }
        if (gain > 0 || removal_dex != 0) {
          // In InterDexReshufflePass, we require gain > 0. For DexRemovalPass,
          // any gain is accepted to increase the possibility of make a dex
          // removable.
          std::lock_guard<std::mutex> lock_guard(mutex);
          if (m_gains_size == m_gains.size()) {
            m_gains.resize(std::max((size_t)1024, m_gains_size * 2));
          }
          m_gains.at(m_gains_size++) = (Move){cls, gain, dex_index};
        }
      }
    });

    m_gains_heap_size = m_gains_size;
    if (m_gains_heap.size() < m_gains_heap_size) {
      m_gains_heap.resize(std::max((size_t)1024, m_gains_heap_size * 2));
    }
    std::iota(m_gains_heap.begin(), m_gains_heap.begin() + m_gains_heap_size,
              0);
    std::make_heap(m_gains_heap.begin(),
                   m_gains_heap.begin() + m_gains_heap_size,
                   compare_indices_by_gains());

    m_epoch += 1;
    m_moves_last_epoch = m_moves_this_epoch;
    m_moves_this_epoch = 0;
    m_also_moved_in_last_epoch = 0;
  }

  std::optional<Move> pop_max_gain() {
    while (m_gains_heap_size > 0) {
      std::pop_heap(m_gains_heap.begin(),
                    m_gains_heap.begin() + m_gains_heap_size,
                    compare_indices_by_gains());
      m_gains_heap_size -= 1;

      const size_t gain_index = m_gains_heap.at(m_gains_heap_size);
      const auto& move = m_gains.at(gain_index);

      auto it = m_move_epoch.find(move.cls);
      if (it != m_move_epoch.end() && it->second >= m_epoch) {
        // Class already moved.
        continue;
      }

      return move;
    }

    return std::nullopt;
  }

  void moved_class(const Move& move) {
    size_t& class_epoch = m_move_epoch[move.cls];
    const bool was_moved_last_epoch = class_epoch == m_epoch - 1;
    class_epoch = m_epoch;

    m_moves_this_epoch += 1;
    m_also_moved_in_last_epoch += was_moved_last_epoch;
    m_moved_classes.emplace(move.cls);
  }

  size_t moves_this_epoch() const { return m_moves_this_epoch; }

  size_t moved_classes_size() const { return m_moved_classes.size(); }

  bool should_stop() const {
    return m_moves_this_epoch == 0 ||
           (static_cast<float>(m_also_moved_in_last_epoch) /
                static_cast<float>(m_moves_last_epoch) >
            0.9f);
  }

  size_t size() const { return m_gains_heap_size; }

  gain_t get_min_gain_val() const { return m_min_gain_val; }

  gain_t compute_move_gain(DexClass* cls,
                           size_t target_index,
                           bool for_removal = false) {
    gain_t gain = 0;
    always_assert(m_class_dex_indices.count(cls));
    auto source_index = m_class_dex_indices.at(cls);
    if (source_index != target_index) {
      always_assert(m_class_refs.count(cls));
      const auto& refs = m_class_refs.at(cls);
      auto& source = m_dexen.at(source_index);
      auto& target = m_dexen.at(target_index);
      for (auto* fref : refs.frefs) {
        auto source_occurrences = source.get_fref_occurrences(fref);
        auto target_occurrences = target.get_fref_occurrences(fref);
        gain +=
            compute_gain(source_occurrences, target_occurrences, for_removal);
      }
      for (auto* mref : refs.mrefs) {
        auto source_occurrences = source.get_mref_occurrences(mref);
        auto target_occurrences = target.get_mref_occurrences(mref);
        gain +=
            compute_gain(source_occurrences, target_occurrences, for_removal);
      }
      for (auto* tref : refs.trefs) {
        auto source_occurrences = source.get_tref_occurrences(tref);
        auto target_occurrences = target.get_tref_occurrences(tref);
        gain +=
            compute_gain(source_occurrences, target_occurrences, for_removal);
      }
      auto& source_strings = m_dexen_strings.at(source_index);
      auto& target_strings = m_dexen_strings.at(target_index);
      for (auto* sref : refs.srefs) {
        auto it = source_strings.find(sref);
        auto source_occurrences = it == source_strings.end() ? 0 : it->second;
        it = target_strings.find(sref);
        auto target_occurrences = it == target_strings.end() ? 0 : it->second;
        gain +=
            compute_gain(source_occurrences, target_occurrences, for_removal);
      }
    }

    return gain;
  }

  gain_t compute_move_gain_after_merging(DexClass* cls,
                                         size_t target_index,
                                         bool for_removal = false) {
    MergerIndex merging_type;
    if (m_class_to_merging_info.count(cls)) {
      merging_type = m_class_to_merging_info.at(cls).merging_type;
    } else {
      // If cls does not belong to any merging type, then use the original
      // formula to compute move gain and multiply it by m_other_weight for
      // non-dedupable references.
      return m_other_weight * compute_move_gain(cls, target_index, for_removal);
    }
    gain_t gain = 0;
    always_assert(m_class_dex_indices.count(cls));
    auto source_index = m_class_dex_indices.at(cls);
    if (source_index != target_index) {
      always_assert(m_class_refs.count(cls));
      const auto& refs = m_class_refs.at(cls);
      auto& source = m_dexen.at(source_index);
      auto& target = m_dexen.at(target_index);
      int source_merging_type_usage =
          source.get_merging_type_usage(merging_type);
      int target_merging_type_usage =
          target.get_merging_type_usage(merging_type);
      for (auto* fref : refs.frefs) {
        // If fref is not defined in cls, then we compute its corresponding gain
        // using the original formula.
        const auto ref_cls = type_class(fref->get_class());
        if (ref_cls != cls) {
          auto source_occurrences = source.get_fref_occurrences(fref);
          auto target_occurrences = target.get_fref_occurrences(fref);
          gain +=
              m_other_weight *
              compute_gain(source_occurrences, target_occurrences, for_removal);
        }
      }
      // We separately compute the gain for frefs *defined in* cls.
      always_assert(m_num_field_defs.count(merging_type));
      gain += m_deduped_weight * m_num_field_defs.at(merging_type) *
              compute_gain(source_merging_type_usage, target_merging_type_usage,
                           for_removal);

      const std::unordered_map<const DexMethod*, MethodGroup>& dedupable_mrefs =
          m_class_to_merging_info.at(cls).dedupable_mrefs;
      for (auto* mref : refs.mrefs) {
        auto source_occurrences = source.get_mref_occurrences(mref);
        auto target_occurrences = target.get_mref_occurrences(mref);
        // If mref is defined in cls, then we use its corresponding merging type
        // method usage in source and target to approximate the
        // source_occurrences and target_occurrences after merging.
        const DexMethod* meth = mref->as_def();
        if (dedupable_mrefs.count(meth)) {
          MethodGroup group = dedupable_mrefs.at(meth);
          source_occurrences =
              source.get_merging_type_method_usage(merging_type, group);
          target_occurrences =
              target.get_merging_type_method_usage(merging_type, group);
          gain +=
              m_deduped_weight *
              compute_gain(source_occurrences, target_occurrences, for_removal);
        } else {
          gain +=
              m_other_weight *
              compute_gain(source_occurrences, target_occurrences, for_removal);
        }
      }
      for (auto* tref : refs.trefs) {
        auto source_occurrences = source.get_tref_occurrences(tref);
        auto target_occurrences = target.get_tref_occurrences(tref);
        const auto ref_cls = type_class(tref);
        if (ref_cls == cls) {
          source_occurrences = source_merging_type_usage;
          target_occurrences = target_merging_type_usage;
        }
        gain += m_other_weight * compute_gain(source_occurrences,
                                              target_occurrences, for_removal);
      }
      auto& source_strings = m_dexen_strings.at(source_index);
      auto& target_strings = m_dexen_strings.at(target_index);
      for (auto* sref : refs.srefs) {
        auto it = source_strings.find(sref);
        auto source_occurrences = it == source_strings.end() ? 0 : it->second;
        it = target_strings.find(sref);
        auto target_occurrences = it == target_strings.end() ? 0 : it->second;
        gain += m_other_weight * compute_gain(source_occurrences,
                                              target_occurrences, for_removal);
      }
    }

    return gain;
  }

  gain_t compute_gain(size_t source_occurrences,
                      size_t target_occurrences,
                      bool for_removal) const {
    if (for_removal) {
      return 0 - power_value_for(target_occurrences);
    }
    return source_occurrences == 0 ? 0
                                   : power_value_for(source_occurrences - 1) -
                                         power_value_for(target_occurrences);
  }

  // The gains improvement values; the class + target dex are a function
  // of the index. It is indices into the gains that are heapified.
  std::vector<Move> m_gains;
  size_t m_gains_size{0};
  std::vector<size_t> m_gains_heap;
  size_t m_gains_heap_size{0};

  // This value is from expriment.
  gain_t m_min_gain_val{-24299166313522127};

  // Tracks when a class was last moved.
  //
  // Epoch advances when gains are recomputed. It starts at 1, so that we
  // are in a state as if every class was moved in epoch 0, and none were
  // moved in epoch 1. Then the first recomputation moves us to epoch 2,
  // so that the stopping criteria doesn't think every class was moved.
  std::unordered_map<DexClass*, size_t> m_move_epoch;
  size_t m_epoch{1};

  // Tracks epoch move counts and inter-epoch move differences.
  size_t m_moves_this_epoch{0};
  size_t m_moves_last_epoch{0};
  size_t m_also_moved_in_last_epoch{0};

  const size_t m_first_dex_index;
  const std::vector<DexClass*>& m_movable_classes;
  const std::unordered_map<DexClass*, size_t>& m_class_dex_indices;
  const std::unordered_map<DexClass*, Refs>& m_class_refs;
  const std::vector<DexStructure>& m_dexen;
  const std::vector<std::unordered_map<const DexString*, size_t>>&
      m_dexen_strings;
  const std::unordered_set<size_t>& m_dynamically_dead_dexes;

  // Class merging related data.
  const std::unordered_map<DexClass*, struct MergingInfo>&
      m_class_to_merging_info;
  const std::unordered_map<MergerIndex, size_t>& m_num_field_defs;
  const bool m_mergeability_aware;
  const gain_t m_deduped_weight{1};
  const gain_t m_other_weight{1};

  // Classes that are already moved once, and should not be moved again. It is
  // used in DexRemovalPass only.
  std::unordered_set<DexClass*> m_moved_classes;
};

class InterDexReshuffleImpl {
 public:
  InterDexReshuffleImpl(
      ConfigFiles& conf,
      PassManager& mgr,
      ReshuffleConfig& config,
      DexClasses& original_scope,
      DexClassesVector& dexen,
      const std::unordered_set<size_t>& dynamically_dead_dexes,
      const boost::optional<class_merging::Model&>& merging_model =
          boost::none);

  void compute_plan();

  void apply_plan();

  bool compute_dex_removal_plan();

 private:
  void print_stats();

  void record_stats();

  bool try_plan_move(const Move& move, bool mergeability_aware = false);

  bool can_move(DexClass* cls);

  size_t get_eliminate_dex(
      const std::unordered_map<size_t, bool>& dex_eliminate);
  ConfigFiles& m_conf;
  PassManager& m_mgr;
  ReshuffleConfig& m_config;
  init_classes::InitClassesWithSideEffects m_init_classes_with_side_effects;
  DexClassesVector& m_dexen;
  const std::unordered_set<size_t>& m_dynamically_dead_dexes;
  boost::optional<class_merging::Model&> m_merging_model;
  size_t m_linear_alloc_limit;
  DexesStructure m_dexes_structure;
  std::vector<DexClass*> m_movable_classes;
  std::unordered_map<DexClass*, size_t> m_class_dex_indices;
  std::unordered_map<DexClass*, Refs> m_class_refs;
  std::vector<DexStructure> m_mutable_dexen;
  std::vector<std::unordered_map<const DexString*, size_t>>
      m_mutable_dexen_strings;
  size_t m_first_dex_index{1}; // skip primary dex
  bool m_order_interdex;
  // Class merging related data.
  std::unordered_map<DexClass*, struct MergingInfo> m_class_to_merging_info;
  std::unordered_map<MergerIndex, size_t> m_num_field_defs;
  bool m_mergeability_aware{false};
};
