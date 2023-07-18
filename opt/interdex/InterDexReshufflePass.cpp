/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterDexReshufflePass.h"

#include "ConfigFiles.h"
#include "DedupStrings.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "DexUtil.h"
#include "InterDexPass.h"
#include "PassManager.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
struct Refs {
  MethodRefs mrefs;
  FieldRefs frefs;
  TypeRefs trefs;
  TypeRefs itrefs;
  std::unordered_set<const DexString*> srefs;
};

const interdex::InterDexPass* get_interdex_pass(const PassManager& mgr) {
  const auto* pass =
      static_cast<interdex::InterDexPass*>(mgr.find_pass("InterDexPass"));
  always_assert_log(pass, "InterDexPass missing");
  return pass;
}

bool can_move(DexClass* cls) {
  return (cls->get_perf_sensitive() != PerfSensitiveGroup::BETAMAP_ORDERED) &&
         !is_canary(cls);
}

// Compute gain powers by reference occurrences. We don't use the upper 20 (19,
// actually, because sign bit) bits to allow for adding all gains of a class.
// TODO: While this integer-based representation allows for a fast and
// deterministic algorithm, it's precision ends at more than 11 occurrences,
// where the gain goes to 0. Based on current experiments, increasing 11 may
// increase the size gain a little bit, but come with a cost of
// “non-determinism” (due to rounding errors or space complexity if we compute
// these differently).
using gain_t = int64_t;
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
                dexen_strings)
      : m_first_dex_index(first_dex_index),
        m_movable_classes(movable_classes),
        m_class_dex_indices(class_dex_indices),
        m_class_refs(class_refs),
        m_dexen(dexen),
        m_dexen_strings(dexen_strings) {}

  void recompute_gains() {
    Timer t("recompute_gains");
    m_gains_size = 0;
    std::mutex mutex;
    walk::parallel::classes(m_movable_classes, [&](DexClass* cls) {
      for (size_t dex_index = m_first_dex_index; dex_index < m_dexen.size();
           ++dex_index) {
        auto gain = compute_move_gain(cls, dex_index);
        if (gain > 0) {
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
  }

  size_t moves_this_epoch() const { return m_moves_this_epoch; }

  bool should_stop() const {
    return m_moves_this_epoch == 0 ||
           (static_cast<float>(m_also_moved_in_last_epoch) /
                static_cast<float>(m_moves_last_epoch) >
            0.9f);
  }

  size_t size() const { return m_gains_heap_size; }

  gain_t compute_move_gain(DexClass* cls, size_t target_index) const {
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
        gain += compute_gain(source_occurrences, target_occurrences);
      }
      for (auto* mref : refs.mrefs) {
        auto source_occurrences = source.get_mref_occurrences(mref);
        auto target_occurrences = target.get_mref_occurrences(mref);
        gain += compute_gain(source_occurrences, target_occurrences);
      }
      for (auto* tref : refs.trefs) {
        auto source_occurrences = source.get_tref_occurrences(tref);
        auto target_occurrences = target.get_tref_occurrences(tref);
        gain += compute_gain(source_occurrences, target_occurrences);
      }
      auto& source_strings = m_dexen_strings.at(source_index);
      auto& target_strings = m_dexen_strings.at(target_index);
      for (auto* sref : refs.srefs) {
        auto it = source_strings.find(sref);
        auto source_occurrences = it == source_strings.end() ? 0 : it->second;
        it = target_strings.find(sref);
        auto target_occurrences = it == target_strings.end() ? 0 : it->second;
        gain += compute_gain(source_occurrences, target_occurrences);
      }
    }

    return gain;
  }

  gain_t compute_gain(size_t source_occurrences,
                      size_t target_occurrences) const {
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
};

class Impl {
 public:
  Impl(ConfigFiles& conf,
       PassManager& mgr,
       InterDexReshufflePass::Config& config,
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
    it = interdex_metrics.find(interdex::METRIC_RESERVED_FREFS);
    m_dexes_structure.set_reserve_frefs(
        (it != interdex_metrics.end() ? it->second : 0) +
        m_config.reserved_extra_frefs);
    it = interdex_metrics.find(interdex::METRIC_RESERVED_TREFS);
    m_dexes_structure.set_reserve_trefs(
        (it != interdex_metrics.end() ? it->second : 0) +
        m_config.reserved_extra_trefs);
    it = interdex_metrics.find(interdex::METRIC_RESERVED_MREFS);
    m_dexes_structure.set_reserve_mrefs(
        (it != interdex_metrics.end() ? it->second : 0) +
        m_config.reserved_extra_mrefs);

    m_mutable_dexen.resize(dexen.size());
    m_mutable_dexen_strings.resize(dexen.size());

    Timer t("init");
    DexClasses classes;
    for (size_t dex_index = m_first_dex_index; dex_index < dexen.size();
         dex_index++) {
      auto& dex = dexen.at(dex_index);
      if (dex_index == m_first_dex_index &&
          !std::any_of(dex.begin(), dex.end(), can_move)) {
        m_first_dex_index++;
        continue;
      }
      for (auto cls : dex) {
        classes.push_back(cls);
        m_class_refs.emplace(cls, Refs());
        if (!can_move(cls)) {
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
            mutable_dex.resolve_init_classes(
                &m_init_classes_with_side_effects, refs.frefs, refs.trefs,
                refs.itrefs, &pending_init_class_fields,
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

  void compute_plan() {
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

  void apply_plan() {
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

 private:
  void print_stats() {
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

  bool try_plan_move(const Move& move) {
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
    auto& target_dex_strings =
        m_mutable_dexen_strings.at(move.target_dex_index);
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

  ConfigFiles& m_conf;
  PassManager& m_mgr;
  InterDexReshufflePass::Config& m_config;
  init_classes::InitClassesWithSideEffects m_init_classes_with_side_effects;
  DexClassesVector& m_dexen;
  size_t m_linear_alloc_limit;
  DexesStructure m_dexes_structure;
  std::vector<DexClass*> m_movable_classes;
  std::unordered_map<DexClass*, size_t> m_class_dex_indices;
  std::unordered_map<DexClass*, Refs> m_class_refs;
  std::vector<DexStructure> m_mutable_dexen;
  std::vector<std::unordered_map<const DexString*, size_t>>
      m_mutable_dexen_strings;
  size_t m_first_dex_index{1}; // skip primary dex
};
} // namespace

void InterDexReshufflePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  const auto* interdex_pass = get_interdex_pass(mgr);
  if (!interdex_pass->minimize_cross_dex_refs()) {
    mgr.incr_metric("no minimize_cross_dex_refs", 1);
    TRACE(
        IDEXR, 1,
        "InterDexReshufflePass not run because InterDexPass is not configured "
        "for minimize_cross_dex_refs.");
    return;
  }

  auto original_scope = build_class_scope(stores);

  auto& root_store = stores.at(0);
  auto& root_dexen = root_store.get_dexen();
  if (root_dexen.size() == 1) {
    // only a primary dex? Nothing to do
    return;
  }

  Impl impl(conf, mgr, m_config, original_scope, root_dexen);
  impl.compute_plan();
  impl.apply_plan();

  // Sanity check
  std::unordered_set<DexClass*> original_scope_set(original_scope.begin(),
                                                   original_scope.end());
  auto new_scope = build_class_scope(stores);
  std::unordered_set<DexClass*> new_scope_set(new_scope.begin(),
                                              new_scope.end());
  always_assert(original_scope_set.size() == new_scope_set.size());
  for (auto cls : original_scope_set) {
    always_assert(new_scope_set.count(cls));
  }
}

static InterDexReshufflePass s_pass;
