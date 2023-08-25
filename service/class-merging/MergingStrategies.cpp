/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergingStrategies.h"

#include "CrossDexRefMinimizer.h"
#include "IRCode.h"
#include "NormalizeConstructor.h"
#include "Show.h"
#include "Trace.h"

namespace class_merging {
namespace strategy {

namespace {

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

  void trace_stats() {
    if (!traceEnabled(CLMG, 5)) {
      return;
    }
    TRACE(CLMG, 5, "============== refs stats ==================");
    for (const auto& stat : refs_stats) {
      TRACE(CLMG, 5, "ref %zu cls %zu", stat.first, stat.second);
    }
    TRACE(CLMG, 5, "group ref %zu code size %zu cls %zu", ref_count,
          estimated_code_size, cls_count);
    TRACE(CLMG, 5, "============================================");
  }
};

size_t estimate_vmethods_code_size(const DexClass* cls) {
  size_t estimated_size = 0;
  for (auto method : cls->get_vmethods()) {
    estimated_size += method->get_code()->estimate_code_units();
  }
  return estimated_size;
}

} // namespace

void MergingStrategy::group_by_cls_count(
    const TypeSet& mergeable_types,
    size_t min_mergeables_count,
    const boost::optional<size_t>& opt_max_mergeables_count,
    const GroupWalkerFn& walker) {
  size_t max_mergeables_count = opt_max_mergeables_count
                                    ? *opt_max_mergeables_count
                                    : std::numeric_limits<size_t>::max();
  redex_assert(min_mergeables_count <= max_mergeables_count &&
               min_mergeables_count >= 2);

  size_t remaining_count = mergeable_types.size();

  auto it = mergeable_types.begin();
  for (; remaining_count >= max_mergeables_count;
       remaining_count -= max_mergeables_count) {
    auto next = std::next(it, max_mergeables_count);
    std::vector<const DexType*> curr_group(it, next);
    walker(curr_group);
    it = next;
  }
  if (remaining_count >= min_mergeables_count) {
    std::vector<const DexType*> curr_group(it, mergeable_types.end());
    walker(curr_group);
  }
}

void MergingStrategy::group_by_code_size(
    const TypeSet& mergeable_types,
    const boost::optional<size_t>& opt_max_mergeables_count,
    const GroupWalkerFn& walker) {
  // 9000 - buffer_for_switch_payload
  constexpr size_t huge_method_split_limit = 8500;

  size_t max_mergeables_count = opt_max_mergeables_count
                                    ? *opt_max_mergeables_count
                                    : std::numeric_limits<size_t>::max();

  std::vector<const DexType*> current_group;

  size_t estimated_merged_code_size = 0;
  for (auto type : mergeable_types) {
    // Only check the code size of vmethods because these vmethods will be
    // merged into a large dispatch, dmethods will be relocated.
    auto vmethod_code_size = estimate_vmethods_code_size(type_class(type));
    if (vmethod_code_size > huge_method_split_limit) {
      // This class will never make it into any group; skip it
      continue;
    }
    if (current_group.size() >= max_mergeables_count) {
      redex_assert(current_group.size() > 1);
      walker(current_group);
      current_group.clear();
      estimated_merged_code_size = 0;
    } else if (estimated_merged_code_size + vmethod_code_size >
               huge_method_split_limit) {
      TRACE(CLMG, 9, "\tgroup_by_code_size %zu classes", current_group.size());
      if (current_group.size() > 1) {
        walker(current_group);
      }
      current_group.clear();
      estimated_merged_code_size = 0;
    }
    current_group.push_back(type);
    estimated_merged_code_size += vmethod_code_size;
  }
  if (current_group.size() > 1) {
    TRACE(CLMG, 9, "\tgroup_by_code_size %zu classes at the end",
          current_group.size());
    walker(current_group);
  }
}

void MergingStrategy::group_by_refs(const TypeSet& mergeable_types,
                                    const GroupWalkerFn& walker) {
  constexpr size_t max_instruction_size = 1 << 15;
  // TODO: Consider making this configurable. It represents the maximum number
  // of non-trivial references (fields, methods, etc.) a group can have before
  // being closed.
  constexpr size_t max_applied_refs = 100;
  constexpr size_t max_refs_per_cls = 50;

  std::vector<const DexType*> current_group;

  Scope mergeable_classes;
  mergeable_classes.reserve(mergeable_types.size());
  for (auto* type : mergeable_types) {
    mergeable_classes.push_back(type_class(type));
    ;
  }
  ClassReferencesCache cache(mergeable_classes);
  cross_dex_ref_minimizer::CrossDexRefMinimizer cross_dex_ref_minimizer({},
                                                                        cache);
  for (auto* cls : mergeable_classes) {
    cross_dex_ref_minimizer.sample(cls);
  }
  for (auto* cls : mergeable_classes) {
    cross_dex_ref_minimizer.insert(cls);
  }
  size_t estimated_merged_code_size = 0;
  size_t current_cls_refs = 0;
  GroupStats group_stats;
  while (!cross_dex_ref_minimizer.empty()) {
    auto curr_cls = current_group.empty() ? cross_dex_ref_minimizer.worst()
                                          : cross_dex_ref_minimizer.front();
    // Only check the code size of vmethods because these vmethods will be
    // merged into a large dispatch, dmethods will be relocated.
    auto vmethod_code_size = estimate_vmethods_code_size(curr_cls);
    auto unapplied_refs_cls =
        cross_dex_ref_minimizer.get_unapplied_refs(curr_cls);
    if (vmethod_code_size > max_instruction_size ||
        unapplied_refs_cls >= max_refs_per_cls) {
      // This class will never make it into any group; skip it
      cross_dex_ref_minimizer.erase(curr_cls, /* emitted */ false,
                                    /* reset */ false);
      continue;
    }
    bool reset = false;
    // If the total code size or total ref count is going to exceed the limit
    // by including the current class, we emit the current group. We also push
    // the current class to the next group.
    if (estimated_merged_code_size + vmethod_code_size > max_instruction_size ||
        cross_dex_ref_minimizer.get_applied_refs() + unapplied_refs_cls >
            max_applied_refs) {
      if (current_group.size() > 1) {
        walker(current_group);
        TRACE(CLMG, 9, "\tgroup_by_refs %zu classes", current_group.size());
        group_stats.trace_stats();
        group_stats.reset();
      }
      current_group.clear();
      estimated_merged_code_size = 0;
      reset = true;
    }
    current_group.push_back(curr_cls->get_type());
    estimated_merged_code_size += vmethod_code_size;
    current_cls_refs =
        cross_dex_ref_minimizer.erase(curr_cls, /* emitted */ true, reset);
    TRACE(CLMG, 5, " curr cls refs %zu %s", current_cls_refs, SHOW(curr_cls));
    group_stats.count(current_cls_refs, vmethod_code_size);
  }
  // Emit what is left in the current group if more than one class, total code
  // size is within limit and total ref count is within limit.
  if (current_group.size() > 1 &&
      estimated_merged_code_size <= max_instruction_size &&
      cross_dex_ref_minimizer.get_applied_refs() <= max_applied_refs) {
    walker(current_group);
    TRACE(CLMG, 9, "\tgroup_by_refs %zu classes at the end",
          current_group.size());
    group_stats.trace_stats();
    group_stats.reset();
  }
}

} // namespace strategy
} // namespace class_merging
