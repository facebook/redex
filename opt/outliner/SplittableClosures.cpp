/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SplittableClosures.h"

#include "ClosureAggregator.h"
#include "ConcurrentContainers.h"
#include "Lazy.h"
#include "Liveness.h"
#include "OutlinerTypeAnalysis.h"
#include "PatriciaTreeSet.h"
#include "ReducedCFGClosureAdapter.h"
#include "Show.h"
#include "StlUtil.h"
#include "Timer.h"
#include "Trace.h"
#include "UninitializedObjects.h"
#include "WorkQueue.h"

namespace {

using namespace method_splitting_impl;
using namespace outliner_impl;
using namespace uninitialized_objects;

struct ScoredClosure {
  cfg::Block* switch_block;
  std::vector<const Closure*> closures;
  std::unordered_set<const ReducedBlock*> reduced_components{};
  size_t split_size{};
  size_t remaining_size{};
  HotSplitKind hot_split_kind{};
  std::vector<ClosureArgument> args{};

  int is_switch() const { return switch_block ? 1 : 0; }

  // id is unique among all splittable closures where is_switch() is the same.
  size_t id() const {
    if (switch_block) {
      return switch_block->id();
    }
    always_assert(closures.size() == 1);
    return closures.front()->target->id();
  }
};

// For a given switch-block/closures, find all incoming preds which will be
// eliminated by splitting them out.
std::unordered_set<const ReducedEdge*> get_except_preds(
    cfg::Block* switch_block,
    const std::vector<const Closure*>& closures,
    const std::unordered_set<const ReducedBlock*>& reduced_components) {
  std::unordered_set<const ReducedEdge*> except_preds;
  for (auto* c : closures) {
    except_preds.insert(c->reduced_block->preds.begin(),
                        c->reduced_block->preds.end());
  }
  if (switch_block) {
    for (auto* c : closures) {
      for (auto* pred : c->reduced_block->preds) {
        if (reduced_components.count(pred->src)) {
          continue;
        }
        for (auto* e : pred->edges) {
          if (e->src() != switch_block) {
            except_preds.erase(pred);
            break;
          }
        }
      }
    }
  }
  return except_preds;
}

std::optional<ScoredClosure> score(
    const Config& config,
    const MethodClosures& mcs,
    float max_overhead_ratio,
    cfg::Block* switch_block,
    const std::vector<const Closure*>& closures) {
  ScoredClosure sc{switch_block, closures};
  for (auto* c : closures) {
    sc.reduced_components.insert(c->reduced_components.begin(),
                                 c->reduced_components.end());
  }
  auto reduced_components_code_size = code_size(sc.reduced_components);
  sc.split_size = config.cost_split_method + reduced_components_code_size;
  if (closures.size() > 1) {
    sc.split_size += config.cost_split_switch +
                     config.cost_split_switch_case * closures.size();
  }
  bool any_src_hot{false};
  bool any_target_hot{false};
  for (auto* c : closures) {
    for (auto* pred : c->reduced_block->preds) {
      any_src_hot |= pred->src->is_hot;
    }
    any_target_hot |= c->reduced_block->is_hot;
  }
  if (any_src_hot) {
    if (any_target_hot) {
      if (sc.split_size < config.min_hot_split_size) {
        return std::nullopt;
      }
      sc.hot_split_kind = HotSplitKind::Hot;
    } else {
      if (sc.split_size < config.min_hot_cold_split_size) {
        return std::nullopt;
      }
      sc.hot_split_kind = HotSplitKind::HotCold;
    }
  } else {
    if (sc.split_size < config.min_cold_split_size) {
      return std::nullopt;
    }
    sc.hot_split_kind = HotSplitKind::Cold;
  }
  always_assert(reduced_components_code_size <= mcs.original_size);
  auto remaining_size_reduction =
      closures.size() > 1
          ? config.cost_split_switch_case * (closures.size() - 1)
          : 0;
  auto estimated_remaining_size =
      mcs.original_size - reduced_components_code_size;
  estimated_remaining_size =
      estimated_remaining_size < remaining_size_reduction
          ? 0
          : estimated_remaining_size - remaining_size_reduction;

  switch (sc.hot_split_kind) {
  case HotSplitKind::Hot:
  case HotSplitKind::HotCold:
    if (estimated_remaining_size < config.min_hot_split_size) {
      return std::nullopt;
    }
    break;
  case HotSplitKind::Cold:
    if (estimated_remaining_size < config.min_cold_split_size) {
      return std::nullopt;
    }
    break;
  default:
    not_reached();
  }

  auto except_preds =
      get_except_preds(switch_block, closures, sc.reduced_components);
  auto& rcfg = *mcs.rcfg;
  auto remaining_blocks = rcfg.reachable(rcfg.entry_block(), except_preds);
  sc.remaining_size = code_size(remaining_blocks);
  sc.remaining_size = sc.remaining_size < remaining_size_reduction
                          ? 0
                          : sc.remaining_size - remaining_size_reduction;
  auto overhead_ratio =
      (sc.split_size + sc.remaining_size) * 1.0 / mcs.original_size - 1.0;
  if (overhead_ratio > max_overhead_ratio) {
    return std::nullopt;
  }
  return std::optional<ScoredClosure>(std::move(sc));
};

std::unordered_set<const ReducedBlock*> get_critical_components(
    const std::vector<std::pair<int32_t, const Closure*>>& keyed,
    const Closure* fallthrough) {
  std::unordered_map<const ReducedBlock*, size_t> counts;
  auto add_to_counts = [&counts](const Closure* c) {
    for (auto* component : c->reduced_components) {
      counts[component]++;
    }
  };
  for (auto [_, c] : keyed) {
    add_to_counts(c);
  }
  std::unordered_set<const ReducedBlock*> critical_components;
  for (auto [c, count] : counts) {
    if (count < keyed.size() && !fallthrough->reduced_components.count(c)) {
      critical_components.insert(c);
    }
  }
  return critical_components;
};

// Find a set of switch cases that are worthwhile to split off
std::optional<ScoredClosure> aggregate(
    const Config& config,
    const MethodClosures& mcs,
    float max_overhead_ratio,
    cfg::Block* switch_block,
    const std::vector<const Closure*>& switched,
    const std::function<bool(const Closure*)>& predicate) {
  always_assert(!switched.empty());
  if (switched.size() == 1) {
    return std::nullopt;
  }
  std::vector<std::pair<int32_t, const Closure*>> keyed; // only considering
                                                         // min-case-key
                                                         // except fallthrough
  const Closure* fallthrough = nullptr; // may include case-keys
  for (auto* c : switched) {
    auto expanded_preds = c->reduced_block->expand_preds(switch_block);
    always_assert(!expanded_preds.empty());
    auto* min_edge = *std::min_element(
        expanded_preds.begin(), expanded_preds.end(),
        [](auto* e, auto* f) { return e->case_key() < f->case_key(); });
    if (min_edge->case_key()) {
      if (predicate(c)) {
        keyed.emplace_back(*min_edge->case_key(), c);
      }
    } else {
      fallthrough = c;
    }
  }
  if (keyed.empty() || !fallthrough) {
    return std::nullopt;
  }
  // Sort to have smallest case keys last, to prefer aggregating a suffix
  // of the (sorted) case keys.
  std::sort(keyed.begin(), keyed.end(),
            [&](auto& p, auto& q) { return p.first > q.first; });
  ClosureAggregator aggregator(get_critical_components(keyed, fallthrough));
  for (auto [_, c] : keyed) {
    aggregator.insert(c);
  }
  std::vector<const Closure*> aggregated{fallthrough};
  // Select the seed case, which will influence all following cases.
  // We start with the largest key, preferring aggregating a suffix.
  auto seed = keyed.front().second;
  aggregator.erase(seed);
  aggregated.push_back(seed);

  // Add up to half of all cases
  while (!aggregator.empty() && aggregated.size() * 2 <= switched.size()) {
    auto c = aggregator.front();
    aggregator.erase(c);
    aggregated.push_back(c);
  }
  while (aggregated.size() > 1) {
    auto opt_sc =
        score(config, mcs, max_overhead_ratio, switch_block, aggregated);
    if (opt_sc) {
      return opt_sc;
    }
    aggregated.pop_back();
  }
  return std::nullopt;
};

// Select closures that meet the configured size thresholds, and score them.
std::vector<ScoredClosure> get_scored_closures(const Config& config,
                                               const MethodClosures& mcs,
                                               float max_overhead_ratio) {
  // For all possible closures, do some quick filtering, and score the
  // surviving ones
  std::vector<ScoredClosure> scored_closures;
  std::unordered_map<cfg::Block*, std::vector<const Closure*>>
      remaining_switch_case_closures;
  for (auto& c : mcs.closures) {
    auto opt_sc = score(config, mcs, max_overhead_ratio,
                        /* switch_block */ nullptr, {&c});
    if (opt_sc) {
      scored_closures.push_back(std::move(*opt_sc));
    }
    for (auto* src : c.srcs) {
      if (src->branchingness() == opcode::BRANCH_SWITCH) {
        remaining_switch_case_closures[src].push_back(&c);
      }
    }
  }

  // Next, try to aggregate switch case closures. We prefer splitting off cases
  // that are all cold, or all hot. Only when we don't find such a set of switch
  // cases, then we'll take anything.
  static std::array<std::function<bool(const Closure*)>, 3> predicates = {
      [](const auto* c) { return !c->reduced_block->is_hot; },
      [](const auto* c) { return c->reduced_block->is_hot; },
      [](const auto*) { return true; }};
  for (auto&& [switch_block, switched] : remaining_switch_case_closures) {
    for (const auto& predicate : predicates) {
      auto opt_sc = aggregate(config, mcs, max_overhead_ratio, switch_block,
                              switched, predicate);
      if (opt_sc) {
        scored_closures.push_back(std::move(*opt_sc));
        break;
      }
    }
  }
  return scored_closures;
};

// Filter out overlapping closures, unsplittable closures (when we cannot
// determine what type we can use as parameter type of a split method), and
// filter out closures which don't meet the configured liveness threshold.
std::vector<SplittableClosure> to_splittable_closures(
    const Config& config,
    const std::shared_ptr<MethodClosures>& mcs,
    std::vector<ScoredClosure> scored_closures) {
  std::vector<SplittableClosure> splittable_closures;
  // We sort closures in a way that allows us to quickly prune contained
  // closures if we found a viable containing closure.
  std::sort(
      scored_closures.begin(), scored_closures.end(), [](auto& a, auto& b) {
        if (a.reduced_components.size() != b.reduced_components.size()) {
          return a.reduced_components.size() > b.reduced_components.size();
        }
        if (a.is_switch() != b.is_switch()) {
          return a.is_switch() < b.is_switch();
        }
        return a.id() < b.id();
      });

  // Now we do the expensive analysis of the remaining scored closures.
  auto method = mcs->method;
  Lazy<OutlinerTypeAnalysis> ota(
      [method] { return std::make_unique<OutlinerTypeAnalysis>(method); });
  auto& rcfg = *mcs->rcfg;
  auto& cfg = method->get_code()->cfg();
  Lazy<LivenessFixpointIterator> liveness_fp_iter([&cfg] {
    auto res = std::make_unique<LivenessFixpointIterator>(cfg);
    res->run({});
    return res;
  });
  Lazy<UninitializedObjectEnvironments> uninitialized_objects([method] {
    return std::make_unique<UninitializedObjectEnvironments>(
        get_uninitialized_object_environments(method));
  });
  Lazy<std::unordered_map<IRInstruction*, const ReducedBlock*>> insns([&rcfg] {
    auto res = std::make_unique<
        std::unordered_map<IRInstruction*, const ReducedBlock*>>();
    for (auto* reduced_block : rcfg.blocks()) {
      for (auto block : reduced_block->blocks) {
        for (auto& mie : InstructionIterable(block)) {
          res->emplace(mie.insn, reduced_block);
        }
      }
    }
    return res;
  });
  Lazy<live_range::DefUseChains> def_uses([&cfg]() {
    return std::make_unique<live_range::DefUseChains>(
        live_range::MoveAwareChains(cfg).get_def_use_chains());
  });
  std::unordered_set<const ReducedBlock*> covered;
  std20::erase_if(scored_closures, [&covered, &ota, &liveness_fp_iter,
                                    &uninitialized_objects, &insns, &def_uses,
                                    &config](auto& sc) {
    for (auto* c : sc.closures) {
      if (covered.count(c->reduced_block)) {
        // We already have this contained closure covered by a valid
        // containing closure
        return true;
      }
    }

    sparta::PatriciaTreeSet<reg_t> live_in;
    for (auto* c : sc.closures) {
      live_in.union_with(
          liveness_fp_iter->get_live_in_vars_at(c->target).elements());
    }
    IRInstruction* first_insn;
    if (sc.switch_block) {
      always_assert(sc.closures.size() > 1);
      first_insn = sc.switch_block->get_last_insn()->insn;
      live_in.insert(first_insn->src(0));
    } else {
      always_assert(sc.closures.size() == 1);
      auto* c = sc.closures.front();
      auto it = c->target->get_first_insn();
      if (it == c->target->end()) {
        return true;
      }
      first_insn = it->insn;
    }
    std::vector<reg_t> ordered_live_in(live_in.begin(), live_in.end());
    if (ordered_live_in.size() > (size_t)config.max_live_in) {
      return true;
    }
    std::sort(ordered_live_in.begin(), ordered_live_in.end());

    ReducedCFGClosureAdapter rcfgca(*ota, first_insn, insns,
                                    sc.reduced_components, def_uses);
    for (auto reg : ordered_live_in) {
      auto defs = rcfgca.get_defs(reg);
      if (defs.size() == 1 && opcode::is_a_const((*defs.begin())->opcode())) {
        sc.args.push_back((ClosureArgument){reg, nullptr, *defs.begin()});
        continue;
      }
      auto type = ota->get_type_demand(rcfgca, reg);
      if (!type) {
        return true;
      }
      sc.args.push_back((ClosureArgument){reg, type, nullptr});
      if (type::is_object(type)) {
        const auto& uninitialized_env = uninitialized_objects->at(first_insn);
        auto opt_uninitialized = uninitialized_env.get(reg).get_constant();
        if (!opt_uninitialized || *opt_uninitialized) {
          return true;
        }
      }
    }

    covered.insert(sc.reduced_components.begin(), sc.reduced_components.end());
    return false;
  });
  // remaining scored closures should be all non-overlapping now
  for (auto& sc : scored_closures) {
    if (traceEnabled(MS, 2)) {
      std::ostringstream oss;
      oss << "=== selected " << show(method) << ": " << sc.split_size << " + "
          << sc.remaining_size << " >= " << mcs->original_size << ", "
          << describe(sc.hot_split_kind) << "\n";
      oss << "   args: ";
      for (auto [reg, type, def] : sc.args) {
        oss << "v" << reg << ":"
            << (type == nullptr ? show(def->opcode()) : show(type)) << ", ";
      }
      oss << "\n";
      oss << "   - ";
      std::vector<const cfg::Block*> blocks;
      std::unordered_set<const ReducedBlock*> reachable;
      for (auto* c : sc.closures) {
        blocks.insert(blocks.end(), c->reduced_block->blocks.begin(),
                      c->reduced_block->blocks.end());
        oss << "R" << c->reduced_block->id << ",";
        auto c_reachable = rcfg.reachable(c->reduced_block);
        reachable.insert(c_reachable.begin(), c_reachable.end());
      }
      oss << ": ";
      std::sort(blocks.begin(), blocks.end(),
                [](auto* b, auto* c) { return b->id() < c->id(); });
      for (auto* b : blocks) {
        oss << "B" << b->id() << ", ";
      }
      oss << "reaches ";
      for (auto* other : reachable) {
        oss << "R" << other->id << ", ";
      }
      oss << "\n";
      oss << ::show(method->get_code()->cfg());
      TRACE(MS, 2, "%s", oss.str().c_str());
    }
    auto rank = sc.split_size * mcs->original_size * 1.0 /
                (sc.split_size + sc.remaining_size);
    auto added_code_size =
        sc.split_size + sc.remaining_size - mcs->original_size;
    splittable_closures.push_back((SplittableClosure){
        mcs, sc.switch_block, std::move(sc.closures), std::move(sc.args), rank,
        added_code_size, sc.hot_split_kind});
  }
  return splittable_closures;
};
} // namespace

namespace method_splitting_impl {
std::unordered_map<DexType*, std::vector<SplittableClosure>>
select_splittable_closures(const std::unordered_set<DexMethod*>& methods,
                           const Config& config) {
  Timer t("select_splittable_closures");
  ConcurrentMap<DexType*, std::vector<SplittableClosure>>
      concurrent_splittable_closures;
  auto concurrent_process_method = [&](DexMethod* method) {
    auto mcs = discover_closures(method, config);
    if (!mcs) {
      return;
    }
    auto& cfg = method->get_code()->cfg();

    std::vector<ScoredClosure> scored_closures;
    auto adjustment = cfg.get_size_adjustment(
        /* assume_no_unreachable_blocks */ true);
    bool is_huge =
        mcs->original_size + adjustment > config.huge_threshold ||
        (method->rstate.too_large_for_inlining_into() &&
         mcs->original_size >= config.min_original_size_too_large_for_inlining);
    auto begin = config.max_overhead_ratio;
    auto end =
        is_huge ? config.max_huge_overhead_ratio : config.max_overhead_ratio;
    for (auto r = begin; scored_closures.empty() && r <= end; r *= 2) {
      scored_closures = get_scored_closures(config, *mcs, r);
    }
    if (scored_closures.empty()) {
      return;
    }
    auto splittable_closures =
        to_splittable_closures(config, mcs, std::move(scored_closures));
    concurrent_splittable_closures.update(
        method->get_class(), [&](auto, auto& v, bool) {
          v.insert(v.end(),
                   std::make_move_iterator(splittable_closures.begin()),
                   std::make_move_iterator(splittable_closures.end()));
        });
  };
  workqueue_run<DexMethod*>(concurrent_process_method, methods);
  return concurrent_splittable_closures.move_to_container();
}

} // namespace method_splitting_impl
