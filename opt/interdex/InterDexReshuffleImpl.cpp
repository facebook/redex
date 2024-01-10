/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexReshuffleImpl.h"
#include "InterDexPass.h"
#include "Show.h"
#include "Trace.h"
#include <boost/algorithm/string/predicate.hpp>

#include <algorithm>
#include <cinttypes>
#include <string>
#include <unordered_set>

/// populate_reshuffable_classes_types collects type of classes that are in cold
/// start but below 20pct appear count. These classes can be considered not as
/// perf sensitive, thus should be movable.
void populate_reshuffable_classes_types(
    std::unordered_set<std::string>& reshuffable_classes, ConfigFiles& conf) {
  const auto& betamap_classes = conf.get_coldstart_classes();
  bool seen_coldstart_20pct_end = false;
  bool seen_coldstart_1pct_end = false;
  constexpr const char* coldstart_20pct_end = "ColdStart20PctEnd";
  constexpr const char* coldstart_1pct_end = "ColdStart1PctEnd";
  for (const auto& cls_name : betamap_classes) {
    if (cls_name.find(coldstart_20pct_end) != std::string::npos) {
      seen_coldstart_20pct_end = true;
    }
    if (cls_name.find(coldstart_1pct_end) != std::string::npos) {
      seen_coldstart_1pct_end = true;
    }
    if (seen_coldstart_20pct_end && !seen_coldstart_1pct_end) {
      reshuffable_classes.insert(cls_name);
    }
  }
}

bool is_reshuffable_class(
    const DexClass* cls,
    const std::unordered_set<std::string>& reshuffable_classes) {
  return reshuffable_classes.find(cls->get_type()->str_copy()) !=
         reshuffable_classes.end();
}

InterDexReshuffleImpl::InterDexReshuffleImpl(ConfigFiles& conf,
                                             PassManager& mgr,
                                             ReshuffleConfig& config,
                                             DexClasses& original_scope,
                                             DexClassesVector& dexen)
    : m_conf(conf),
      m_mgr(mgr),
      m_config(config),
      m_init_classes_with_side_effects(original_scope,
                                       conf.create_init_class_insns()),
      m_dexen(dexen) {
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
  std::unordered_set<std::string> reshuffable_classes;
  if (config.exclude_below20pct_coldstart_classes) {
    populate_reshuffable_classes_types(reshuffable_classes, conf);
  }
  for (size_t dex_index = m_first_dex_index; dex_index < dexen.size();
       dex_index++) {
    auto& dex = dexen.at(dex_index);
    if (dex_index == m_first_dex_index &&
        !std::any_of(dex.begin(), dex.end(),
                     [this, &reshuffable_classes](auto* cls) {
                       return can_move(cls) ||
                              is_reshuffable_class(cls, reshuffable_classes);
                     })) {
      m_first_dex_index++;
      continue;
    }
    for (auto cls : dex) {
      classes.push_back(cls);
      m_class_refs.emplace(cls, Refs());
      if (!can_move(cls) && !is_reshuffable_class(cls, reshuffable_classes)) {
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
}

void InterDexReshuffleImpl::compute_plan() {
  Timer t("compute_plan");
  MoveGains move_gains(m_first_dex_index, m_movable_classes,
                       m_class_dex_indices, m_class_refs, m_mutable_dexen,
                       m_mutable_dexen_strings);
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
      auto recomputed_gain =
          move_gains.compute_move_gain(move.cls, move.target_dex_index);
      if (recomputed_gain <= 0) {
        continue;
      }

      // Check if it is a valid move.
      if (!try_plan_move(move)) {
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
  TRACE(IDEXR, 1, "executed %zu moves in %zu batches", total_moves, batches);
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

void InterDexReshuffleImpl::print_stats() {
  size_t n_classes = 0;
  size_t n_mrefs = 0;
  size_t n_frefs = 0;
  for (size_t idx = 0; idx < m_mutable_dexen.size(); ++idx) {
    auto& mutable_dex = m_mutable_dexen.at(idx);
    n_classes += mutable_dex.get_num_classes();
    n_mrefs += mutable_dex.get_num_mrefs();
    n_frefs += mutable_dex.get_num_frefs();
  }

  TRACE(IDEXR, 5, "Global stats:");
  TRACE(IDEXR, 5, "\t %zu classes", n_classes);
  TRACE(IDEXR, 5, "\t %zu mrefs", n_mrefs);
  TRACE(IDEXR, 5, "\t %zu frefs", n_frefs);
}

bool InterDexReshuffleImpl::try_plan_move(const Move& move) {
  auto& target_dex = m_mutable_dexen.at(move.target_dex_index);
  always_assert(m_class_refs.count(move.cls));
  const auto& refs = m_class_refs.at(move.cls);
  TypeRefs pending_init_class_fields;
  TypeRefs pending_init_class_types;
  target_dex.resolve_init_classes(
      &m_init_classes_with_side_effects, refs.frefs, refs.trefs, refs.itrefs,
      &pending_init_class_fields, &pending_init_class_types);
  auto laclazz = estimate_linear_alloc(move.cls);
  if (!target_dex.add_class_if_fits(
          refs.mrefs, refs.frefs, refs.trefs, pending_init_class_fields,
          pending_init_class_types, m_linear_alloc_limit,
          m_dexes_structure.get_frefs_limit(),
          m_dexes_structure.get_mrefs_limit(),
          m_dexes_structure.get_trefs_limit(), move.cls)) {
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
  return true;
}

bool InterDexReshuffleImpl::can_move(DexClass* cls) {
  return (!m_order_interdex ||
          cls->get_perf_sensitive() != PerfSensitiveGroup::BETAMAP_ORDERED) &&
         !is_canary(cls);
}
