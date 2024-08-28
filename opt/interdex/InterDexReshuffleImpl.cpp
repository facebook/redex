/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexReshuffleImpl.h"
#include "ClassMerging.h"
#include "InterDexPass.h"
#include "Show.h"
#include "Trace.h"
#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <cinttypes>
#include <string>
#include <unordered_set>

/// populate_reshufflable_classes_types collects all classes that appear
/// with less than or equal to interaction_frequency_threshold percent of the
/// time in all interactions the class appears in
void populate_reshufflable_classes_types(
    std::unordered_set<const DexString*>& reshufflable_classes,
    ConfigFiles& conf,
    size_t interaction_frequency_threshold) {
  const auto& class_freqs = conf.get_class_frequencies();
  for (const auto& class_freq : class_freqs) {
    const std::vector<uint8_t>& frequencies = class_freq.second;
    bool below_threshold = true;
    for (auto freq : frequencies) {
      if (freq > interaction_frequency_threshold) {
        below_threshold = false;
      }
    }
    if (below_threshold) {
      reshufflable_classes.insert(class_freq.first);
    }
  }
}

bool is_reshufflable_class(
    const DexClass* cls,
    const std::unordered_set<const DexString*>& reshufflable_classes) {
  return reshufflable_classes.find(cls->get_type()->get_name()) !=
         reshufflable_classes.end();
}

InterDexReshuffleImpl::InterDexReshuffleImpl(
    ConfigFiles& conf,
    PassManager& mgr,
    ReshuffleConfig& config,
    DexClasses& original_scope,
    DexClassesVector& dexen,
    const std::unordered_set<size_t>& dynamically_dead_dexes,
    const boost::optional<class_merging::Model&>& merging_model)
    : m_conf(conf),
      m_mgr(mgr),
      m_config(config),
      m_init_classes_with_side_effects(original_scope,
                                       conf.create_init_class_insns()),
      m_dexen(dexen),
      m_dynamically_dead_dexes(dynamically_dead_dexes),
      m_merging_model(merging_model) {
  m_dexes_structure.set_min_sdk(mgr.get_redex_options().min_sdk);
  const auto& interdex_metrics = mgr.get_interdex_metrics();
  auto it = interdex_metrics.find(interdex::METRIC_LINEAR_ALLOC_LIMIT);
  m_linear_alloc_limit = (it != interdex_metrics.end() ? it->second : 0) +
                         m_config.extra_linear_alloc_limit;
  it = interdex_metrics.find(interdex::METRIC_ORDER_INTERDEX);
  m_order_interdex = it == interdex_metrics.end() || it->second;
  auto refs_info = mgr.get_reserved_refs();
  m_dexes_structure.set_reserve_frefs(refs_info.frefs +
                                      m_config.reserved_extra_frefs);
  m_dexes_structure.set_reserve_trefs(refs_info.trefs +
                                      m_config.reserved_extra_trefs);
  m_dexes_structure.set_reserve_mrefs(refs_info.mrefs +
                                      m_config.reserved_extra_mrefs);

  m_mutable_dexen.resize(dexen.size());
  m_mutable_dexen_strings.resize(dexen.size());

  Timer t("init");
  DexClasses classes;
  std::unordered_set<const DexString*> reshufflable_classes;
  if (config.exclude_below20pct_coldstart_classes) {
    populate_reshufflable_classes_types(reshufflable_classes, conf,
                                        config.interaction_frequency_threshold);
  }
  m_mgr.incr_metric("num_reshufflable_classes", reshufflable_classes.size());
  for (size_t dex_index = m_first_dex_index; dex_index < dexen.size();
       dex_index++) {
    auto& dex = dexen.at(dex_index);
    if (dex_index == m_first_dex_index &&
        !std::any_of(dex.begin(), dex.end(),
                     [this, &reshufflable_classes](auto* cls) {
                       return can_move(cls) ||
                              is_reshufflable_class(cls, reshufflable_classes);
                     })) {
      m_first_dex_index++;
      continue;
    }
    for (auto cls : dex) {
      classes.push_back(cls);
      m_class_refs.emplace(cls, Refs());
      if (!can_move(cls) && !is_reshufflable_class(cls, reshufflable_classes)) {
        continue;
      }
      m_movable_classes.push_back(cls);
      m_class_dex_indices.emplace(cls, dex_index);
    }
  }
  walk::parallel::classes(classes, [&](DexClass* cls) {
    always_assert(m_class_refs.count(cls));
    auto& refs = m_class_refs.at(cls);
    cls->gather_methods(refs.mrefs);
    cls->gather_fields(refs.frefs);
    cls->gather_types(refs.trefs);
    std::vector<DexType*> itrefs;
    cls->gather_init_classes(itrefs);
    refs.itrefs.insert(itrefs.begin(), itrefs.end());
    cls->gather_strings(refs.srefs);
  });
  workqueue_run_for<size_t>(
      m_first_dex_index, dexen.size(), [&](size_t dex_idx) {
        auto& dex = dexen.at(dex_idx);
        auto& mutable_dex = m_mutable_dexen.at(dex_idx);
        auto& mutable_dex_strings = m_mutable_dexen_strings.at(dex_idx);
        for (auto cls : dex) {
          always_assert(m_class_refs.count(cls));
          const auto& refs = m_class_refs.at(cls);
          TypeRefs pending_init_class_fields;
          TypeRefs pending_init_class_types;
          mutable_dex.resolve_init_classes(&m_init_classes_with_side_effects,
                                           refs.frefs, refs.trefs, refs.itrefs,
                                           &pending_init_class_fields,
                                           &pending_init_class_types);
          auto laclazz = estimate_linear_alloc(cls);
          mutable_dex.add_class_no_checks(
              refs.mrefs, refs.frefs, refs.trefs, pending_init_class_fields,
              pending_init_class_types, laclazz, cls);
          for (auto* sref : refs.srefs) {
            mutable_dex_strings[sref]++;
          }
        }
      });

  // Initialize m_class_to_merging_info and m_num_field_defs
  if (m_merging_model == boost::none) {
    return;
  }

  m_mergeability_aware = true;
  MergerIndex num_merging_types = 0;
  m_merging_model->walk_hierarchy([&](const class_merging::MergerType& merger) {
    if (!merger.has_mergeables()) {
      return;
    }
    for (const DexType* mergeable : merger.mergeables) {
      const auto cls = type_class(mergeable);
      if (cls != nullptr) {
        m_class_to_merging_info.emplace(cls, MergingInfo());
        auto& merging_info = m_class_to_merging_info.at(cls);
        merging_info.merging_type = num_merging_types;
      }
    }
    MethodGroup group = 0;
    if (!merger.vmethods.empty()) {
      for (const auto& vmeths : merger.vmethods) {
        for (const auto* meth : vmeths.overrides) {
          const auto meth_cls = type_class(meth->get_class());
          auto& merging_info = m_class_to_merging_info.at(meth_cls);
          merging_info.dedupable_mrefs[meth] = group;
        }
        group++;
      }
    }
    if (!merger.intfs_methods.empty()) {
      for (const auto& intf_meths : merger.intfs_methods) {
        for (const auto* meth : intf_meths.methods) {
          const auto meth_cls = type_class(meth->get_class());
          auto& merging_info = m_class_to_merging_info.at(meth_cls);
          merging_info.dedupable_mrefs[meth] = group;
        }
        group++;
      }
    }

    m_num_field_defs.emplace(num_merging_types, merger.shape.field_count());
    num_merging_types++;
  });

  // Initialize hypothetical class merging stats in DexStructure.
  for (size_t dex_idx = m_first_dex_index; dex_idx < dexen.size(); dex_idx++) {
    auto& dex = dexen.at(dex_idx);
    auto& mutable_dex = m_mutable_dexen.at(dex_idx);
    std::unordered_map<MergerIndex, std::unordered_map<MethodGroup, size_t>>
        merging_type_method_usage;
    std::unordered_map<MergerIndex, size_t> merging_type_usage;
    int num_new_methods = 0;
    int num_deduped_methods = 0;
    for (auto cls : dex) {
      if (m_class_to_merging_info.count(cls)) {
        const auto& merging_info = m_class_to_merging_info.at(cls);
        MergerIndex merging_type = merging_info.merging_type;
        merging_type_usage[merging_type]++;
        for (const auto& method : merging_info.dedupable_mrefs) {
          MethodGroup group = method.second;
          merging_type_method_usage[merging_type][group]++;
          num_deduped_methods++;
        }
      }
    }
    for (const auto& entry : merging_type_method_usage) {
      num_new_methods += entry.second.size();
    }
    mutable_dex.set_merging_type_usage(merging_type_usage);
    mutable_dex.set_merging_type_method_usage(merging_type_method_usage);
    mutable_dex.set_num_new_methods(num_new_methods);
    mutable_dex.set_num_deduped_methods(num_deduped_methods);
  }
}

size_t InterDexReshuffleImpl::get_eliminate_dex(
    const std::unordered_map<size_t, bool>& dex_eliminate) {
  size_t e_dex_idx = 0;
  size_t mrefs_limit = m_dexes_structure.get_mrefs_limit();
  size_t frefs_limit = m_dexes_structure.get_frefs_limit();
  size_t mrefs = mrefs_limit;
  size_t frefs = frefs_limit;
  size_t mrefs_avail = 0;
  size_t frefs_avail = 0;
  TRACE(IDEXR, 1, "mutable dexen are %zu, and orignial dexen are %zu\n",
        m_mutable_dexen.size(), m_dexen.size());
  // Step1: find out the dex with the smallest refs.
  for (size_t dex_index = m_first_dex_index; dex_index < m_mutable_dexen.size();
       dex_index++) {
    auto& cur = m_mutable_dexen.at(dex_index);
    auto cur_mrefs = cur.get_num_mrefs();
    auto cur_frefs = cur.get_num_frefs();
    mrefs_avail += (mrefs_limit - cur_mrefs);
    frefs_avail += (frefs_limit - cur_frefs);
    if (!dex_eliminate.at(dex_index)) {
      continue;
    }
    TRACE(IDEXR, 1, "In dex %zu, mrefs is %zu, frefs is %zu\n", dex_index,
          cur_mrefs, cur_frefs);
    if (cur_mrefs > mrefs) {
      continue;
    }
    if (cur_mrefs < mrefs) {
      mrefs = cur_mrefs;
      frefs = cur_frefs;
      e_dex_idx = dex_index;
      continue;
    }
    if (cur_frefs < frefs) {
      mrefs = cur_mrefs;
      frefs = cur_frefs;
      e_dex_idx = dex_index;
    }
  }

  // Step2. Check if the rest of dexes have enough space for classes moving in
  // a high level.
  mrefs_avail -= (mrefs_limit - mrefs);
  frefs_avail -= (frefs_limit - frefs);
  if (mrefs_avail <= mrefs || frefs_avail <= frefs) {
    return 0;
  }
  return e_dex_idx;
}

void InterDexReshuffleImpl::compute_plan() {
  Timer t("compute_plan");
  MoveGains move_gains(
      m_first_dex_index, m_movable_classes, m_class_dex_indices, m_class_refs,
      m_mutable_dexen, m_mutable_dexen_strings, m_dynamically_dead_dexes,
      m_class_to_merging_info, m_num_field_defs, m_mergeability_aware,
      m_config.deduped_weight, m_config.other_weight);
  size_t batches{0};
  size_t total_moves{0};
  size_t max_move_gains{0};
  for (; batches < m_config.max_batches; batches++) {
    Timer u("batch");
    move_gains.recompute_gains();
    max_move_gains = std::max(max_move_gains, move_gains.size());

    while (move_gains.moves_this_epoch() < m_config.max_batch_size) {
      std::optional<Move> move_opt = move_gains.pop_max_gain();
      if (!move_opt) {
        break;
      }

      const Move& move = *move_opt;
      gain_t recomputed_gain = 0;
      if (m_mergeability_aware) {
        recomputed_gain = move_gains.compute_move_gain_after_merging(
            move.cls, move.target_dex_index);
      } else {
        recomputed_gain =
            move_gains.compute_move_gain(move.cls, move.target_dex_index);
      }
      if (recomputed_gain <= 0) {
        continue;
      }

      // Check if it is a valid move.
      if (!try_plan_move(move, /*mergeability_aware=*/m_mergeability_aware)) {
        continue;
      }
      if (traceEnabled(IDEXR, 5)) {
        print_stats();
      }
      move_gains.moved_class(move);
    }
    total_moves += move_gains.moves_this_epoch();
    TRACE(IDEXR, 2, "executed %zu moves in epoch %zu",
          move_gains.moves_this_epoch(), batches);
    if (move_gains.should_stop()) {
      break;
    }
  }

  m_mgr.incr_metric("max_move_gains", max_move_gains);
  m_mgr.incr_metric("total_moves", total_moves);
  m_mgr.incr_metric("batches", batches);
  m_mgr.incr_metric("first_dex_index", m_first_dex_index);
  record_stats();
  TRACE(IDEXR, 1, "executed %zu moves in %zu batches", total_moves, batches);
}

bool InterDexReshuffleImpl::compute_dex_removal_plan() {
  Timer t("compute_dex_removal_plan");
  std::unordered_map<size_t, bool> dex_eliminate;

  for (size_t dex_index = m_first_dex_index; dex_index < m_dexen.size();
       dex_index++) {
    dex_eliminate.emplace(dex_index, true);
    auto& dex = m_dexen.at(dex_index);
    for (auto cls : dex) {
      if (!can_move(cls)) {
        if (!is_canary(cls)) {
          // If a dex contains any non-canary classes which couldn't be
          // moved, this dex can not be eliminated.
          dex_eliminate.at(dex_index) = false;
          break;
        }
      }
    }
  }

  size_t removal_dex = get_eliminate_dex(dex_eliminate);
  if (removal_dex == 0) {
    // No dex can be removed.
    return false;
  }

  TRACE(IDEXR, 1, "Checking if %zu could be removed", removal_dex);
  std::vector<DexClass*> movable_classes;
  std::unordered_map<DexClass*, size_t> class_dex_indices;
  auto& dex = m_dexen.at(removal_dex);
  for (auto cls : dex) {
    if (is_canary(cls)) {
      continue;
    }
    movable_classes.push_back(cls);
    class_dex_indices.emplace(cls, removal_dex);
  }

  MoveGains move_gains(m_first_dex_index, movable_classes, class_dex_indices,
                       m_class_refs, m_mutable_dexen, m_mutable_dexen_strings,
                       m_dynamically_dead_dexes, m_class_to_merging_info,
                       m_num_field_defs, m_mergeability_aware,
                       m_config.deduped_weight, m_config.other_weight);
  size_t max_move_gains{0};

  size_t max_batch = movable_classes.size();
  size_t batches{0};

  while (batches < max_batch) {
    batches++;
    move_gains.recompute_gains(removal_dex);
    max_move_gains = std::max(max_move_gains, move_gains.size());
    while (move_gains.moved_classes_size() < movable_classes.size()) {
      std::optional<Move> move_opt = move_gains.pop_max_gain();
      if (!move_opt) {
        break;
      }
      const Move& move = *move_opt;
      // Check if it is a valid move.
      if (!try_plan_move(move)) {
        continue;
      }
      if (traceEnabled(IDEXR, 5)) {
        print_stats();
      }
      move_gains.moved_class(move);
      TRACE(IDEXR, 2, "Move class %s to Dex %zu", SHOW(move.cls),
            move.target_dex_index);
    }
    if (move_gains.moved_classes_size() == movable_classes.size()) {
      // All expected classes have been moved.
      break;
    }
  }

  if (move_gains.moved_classes_size() != movable_classes.size()) {
    TRACE(IDEXR, 1, "Dex removal failed, still %zu classes left",
          movable_classes.size() - move_gains.moved_classes_size());
    return false;
  }

  m_mgr.incr_metric("max_move_gains", max_move_gains);
  m_mgr.incr_metric("total_moved_classes", move_gains.moved_classes_size());
  record_stats();
  return true;
}

void InterDexReshuffleImpl::apply_plan() {
  Timer t("finish");
  workqueue_run_for<size_t>(
      m_first_dex_index, m_mutable_dexen.size(), [&](size_t dex_idx) {
        auto& dex = m_dexen.at(dex_idx);
        const auto& mutable_dex = m_mutable_dexen.at(dex_idx);
        auto classes = mutable_dex.get_classes(/* perf_based */ true);
        TRACE(IDEXR, 2, "dex %zu: %zu => %zu classes", dex_idx, dex.size(),
              classes.size());
        dex = std::move(classes);
      });
}

void InterDexReshuffleImpl::record_stats() {
  for (size_t idx = 0; idx < m_mutable_dexen.size(); ++idx) {
    auto& mutable_dex = m_mutable_dexen.at(idx);
    m_mgr.set_metric("Dex" + std::to_string(idx) + "number_of_mrefs",
                     mutable_dex.get_num_mrefs());
  }
}

void InterDexReshuffleImpl::print_stats() {
  size_t n_classes = 0;
  size_t n_mrefs = 0;
  size_t n_frefs = 0;
  for (size_t idx = 0; idx < m_mutable_dexen.size(); ++idx) {
    auto& mutable_dex = m_mutable_dexen.at(idx);
    n_classes += mutable_dex.get_num_classes();
    n_mrefs += mutable_dex.get_num_mrefs();
    n_frefs += mutable_dex.get_num_frefs();
    TRACE(IDEXR, 5, "Global stats for dex %zu:", idx);
    TRACE(IDEXR, 5, "\t %zu classes", mutable_dex.get_num_classes());
    TRACE(IDEXR, 5, "\t %zu mrefs", mutable_dex.get_num_mrefs());
    TRACE(IDEXR, 5, "\t %zu frefs", mutable_dex.get_num_frefs());
  }

  TRACE(IDEXR, 5, "Global stats:");
  TRACE(IDEXR, 5, "\t %zu classes", n_classes);
  TRACE(IDEXR, 5, "\t %zu mrefs", n_mrefs);
  TRACE(IDEXR, 5, "\t %zu frefs", n_frefs);
}

bool InterDexReshuffleImpl::try_plan_move(const Move& move,
                                          bool mergeability_aware) {
  bool special_case =
      mergeability_aware && m_class_to_merging_info.count(move.cls);
  auto& target_dex = m_mutable_dexen.at(move.target_dex_index);
  always_assert(m_class_refs.count(move.cls));
  const auto& refs = m_class_refs.at(move.cls);
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;
  target_dex.resolve_init_classes(
      &m_init_classes_with_side_effects, refs.frefs, refs.trefs, refs.itrefs,
      &pending_init_class_fields, &pending_init_class_types);
  auto laclazz = estimate_linear_alloc(move.cls);

  MergerIndex merging_type;
  size_t clazz_num_dedupable_method_defs = 0;

  if (special_case) {
    merging_type = m_class_to_merging_info.at(move.cls).merging_type;
    // Compute the number of method definitions in move.cls that can be deduped
    // in target_dex.
    const std::unordered_map<const DexMethod*, MethodGroup>& dedupable_mrefs =
        m_class_to_merging_info.at(move.cls).dedupable_mrefs;
    for (const auto& method : dedupable_mrefs) {
      MethodGroup group = method.second;
      const size_t old_usage =
          target_dex.get_merging_type_method_usage(merging_type, group);
      if (old_usage > 0) {
        clazz_num_dedupable_method_defs++;
      }
    }
  }

  if (!target_dex.add_class_if_fits(
          refs.mrefs, refs.frefs, refs.trefs, pending_init_class_fields,
          pending_init_class_types, m_linear_alloc_limit,
          m_dexes_structure.get_frefs_limit(),
          m_dexes_structure.get_mrefs_limit(),
          m_dexes_structure.get_trefs_limit(), move.cls,
          /*mergeability_aware=*/
          m_mergeability_aware, /*clazz_num_dedupable_method_defs=*/
          clazz_num_dedupable_method_defs)) {
    return false;
  }
  auto& target_dex_strings = m_mutable_dexen_strings.at(move.target_dex_index);
  for (auto* sref : refs.srefs) {
    target_dex_strings[sref]++;
  }
  always_assert(m_class_dex_indices.count(move.cls));
  auto& dex_index = m_class_dex_indices.at(move.cls);
  auto& source_dex = m_mutable_dexen.at(dex_index);
  source_dex.remove_class(&m_init_classes_with_side_effects, refs.mrefs,
                          refs.frefs, refs.trefs, pending_init_class_fields,
                          pending_init_class_types, laclazz, move.cls);
  auto& source_dex_strings = m_mutable_dexen_strings.at(dex_index);
  for (auto* sref : refs.srefs) {
    auto it = source_dex_strings.find(sref);
    if (--it->second == 0) {
      source_dex_strings.erase(it);
    }
  }
  dex_index = move.target_dex_index;

  if (special_case) {
    // Update class merging stats in source_dex and target_dex
    source_dex.decrease_merging_type_usage(merging_type);
    target_dex.increase_merging_type_usage(merging_type);
    const std::unordered_map<const DexMethod*, MethodGroup>& dedupable_mrefs =
        m_class_to_merging_info.at(move.cls).dedupable_mrefs;
    for (const auto& method : dedupable_mrefs) {
      MethodGroup group = method.second;
      // Source_dex updates
      const int source_old_usage =
          source_dex.get_merging_type_method_usage(merging_type, group);
      always_assert(source_old_usage > 0);
      source_dex.decrease_merging_type_method_usage(merging_type, group);
      if (source_old_usage == 1) {
        source_dex.decrease_num_new_methods();
      }
      source_dex.decrease_num_deduped_methods();
      // Target_dex updates
      const int target_old_usage =
          target_dex.get_merging_type_method_usage(merging_type, group);
      target_dex.increase_merging_type_method_usage(merging_type, group);
      if (target_old_usage == 0) {
        target_dex.increase_num_new_methods();
      }
      target_dex.increase_num_deduped_methods();
    }
  }
  return true;
}

bool InterDexReshuffleImpl::can_move(DexClass* cls) {
  return (!m_order_interdex ||
          cls->get_perf_sensitive() != PerfSensitiveGroup::BETAMAP_ORDERED) &&
         !is_canary(cls) &&
         (m_dynamically_dead_dexes.empty() || !cls->is_dynamically_dead());
}
