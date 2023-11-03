/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PerfMethodInlinePass.h"

#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/algorithm/transform.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <queue>

#include "ConfigFiles.h"
#include "ControlFlow.h"
#include "CppUtil.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "InlineForSpeed.h"
#include "Macros.h"
#include "MethodInliner.h"
#include "MethodProfiles.h"
#include "PGIForest.h"
#include "RedexContext.h"
#include "Resolver.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"

namespace {

using namespace method_profiles;

class InlineForSpeedBase : public InlineForSpeed {
 public:
  bool should_inline_generic(const DexMethod* caller_method,
                             const DexMethod* callee_method) final {
    bool accept = should_inline_impl(caller_method, callee_method);
    ++m_num_choices;
    if (accept) {
      ++m_num_accepted;
    }
    return accept;
  }

  size_t get_num_choices() const { return m_num_choices; }
  size_t get_num_accepted() const { return m_num_accepted; }

  bool should_inline_callsite(const DexMethod* caller_method,
                              const DexMethod* callee_method,
                              const cfg::Block* caller_block) final {
    bool accept =
        should_inline_callsite_impl(caller_method, callee_method, caller_block);
    ++m_num_callsite_choices;
    if (accept) {
      ++m_num_callsite_accepted;
    }
    return accept;
  }

  size_t get_num_callsite_choices() const { return m_num_callsite_choices; }
  size_t get_num_callsite_accepted() const { return m_num_callsite_accepted; }

 protected:
  virtual bool should_inline_impl(const DexMethod* caller_method,
                                  const DexMethod* callee_method) = 0;
  virtual bool should_inline_callsite_impl(const DexMethod* caller_method,
                                           const DexMethod* callee_method,
                                           const cfg::Block* caller_block) = 0;

  size_t m_num_choices{0};
  size_t m_num_accepted{0};

  size_t m_num_callsite_choices{0};
  size_t m_num_callsite_accepted{0};
};

class InlineForSpeedMethodProfiles final : public InlineForSpeedBase {
 public:
  explicit InlineForSpeedMethodProfiles(const MethodProfiles* method_profiles)
      : m_method_profiles(method_profiles) {
    compute_hot_methods();
  }

 protected:
  bool should_inline_impl(const DexMethod* caller_method,
                          const DexMethod* callee_method) override;

  bool should_inline_callsite_impl(
      const DexMethod* caller_method ATTRIBUTE_UNUSED,
      const DexMethod* callee_method ATTRIBUTE_UNUSED,
      const cfg::Block* caller_block ATTRIBUTE_UNUSED) final {
    return true;
  }

 private:
  void compute_hot_methods();
  bool should_inline_per_interaction(const DexMethod* caller_method,
                                     const DexMethod* callee_method,
                                     uint32_t caller_insns,
                                     uint32_t callee_insns,
                                     const std::string& interaction_id,
                                     const StatsMap& method_stats) const;

  const MethodProfiles* m_method_profiles;
  std::map<std::string, std::pair<double, double>> m_min_scores;
};

constexpr double MIN_APPEAR_PERCENT = 80.0;

void InlineForSpeedMethodProfiles::compute_hot_methods() {
  if (m_method_profiles == nullptr || !m_method_profiles->has_stats()) {
    return;
  }
  for (const auto& pair : m_method_profiles->all_interactions()) {
    const std::string& interaction_id = pair.first;
    const auto& method_stats = pair.second;
    size_t popular_set_size = 0;
    for (const auto& entry : method_stats) {
      if (entry.second.appear_percent >= MIN_APPEAR_PERCENT) {
        ++popular_set_size;
      }
    }
    // Methods in the top PERCENTILE of call counts will be considered warm/hot.
    constexpr double WARM_PERCENTILE = 0.25;
    constexpr double HOT_PERCENTILE = 0.1;
    // Find the lowest score that is within the given percentile
    constexpr size_t MIN_SIZE = 1;
    size_t warm_size = std::max(
        MIN_SIZE, static_cast<size_t>(popular_set_size * WARM_PERCENTILE));
    size_t hot_size = std::max(
        MIN_SIZE, static_cast<size_t>(popular_set_size * HOT_PERCENTILE));
    // the "top" of the queue is actually the minimum warm/hot score
    using pq =
        std::priority_queue<double, std::vector<double>, std::greater<double>>;
    pq warm_scores;
    pq hot_scores;
    auto maybe_push = [](pq& q, size_t size, double value) {
      if (q.size() < size) {
        q.push(value);
      } else if (value > q.top()) {
        q.push(value);
        q.pop();
      }
    };
    for (const auto& entry : method_stats) {
      const auto& stat = entry.second;
      if (stat.appear_percent >= MIN_APPEAR_PERCENT) {
        auto score = stat.call_count;
        maybe_push(warm_scores, warm_size, score);
        maybe_push(hot_scores, hot_size, score);
      }
    }
    double min_warm_score = std::max(50.0, warm_scores.top());
    double min_hot_score = std::max(100.0, hot_scores.top());
    TRACE(METH_PROF,
          2,
          "%s min scores = %f, %f",
          interaction_id.c_str(),
          min_warm_score,
          min_hot_score);
    std::pair<double, double> p(min_warm_score, min_hot_score);
    m_min_scores.emplace(interaction_id, std::move(p));
  }
}

bool InlineForSpeedMethodProfiles::should_inline_impl(
    const DexMethod* caller_method, const DexMethod* callee_method) {
  auto caller_insns = caller_method->get_code()->cfg().num_opcodes();
  // The cost of inlining large methods usually outweighs the benefits
  constexpr uint32_t MAX_NUM_INSNS = 240;
  if (caller_insns > MAX_NUM_INSNS) {
    return false;
  }
  auto callee_insns = callee_method->get_code()->cfg().num_opcodes();
  if (callee_insns > MAX_NUM_INSNS) {
    return false;
  }

  // If the pair is hot under any interaction, inline it.
  for (const auto& pair : m_method_profiles->all_interactions()) {
    bool should = should_inline_per_interaction(caller_method,
                                                callee_method,
                                                caller_insns,
                                                callee_insns,
                                                pair.first,
                                                pair.second);
    if (should) {
      return true;
    }
  }
  return false;
}

bool InlineForSpeedMethodProfiles::should_inline_per_interaction(
    const DexMethod* caller_method,
    const DexMethod* callee_method,
    uint32_t caller_insns,
    uint32_t callee_insns,
    const std::string& interaction_id,
    const StatsMap& method_stats) const {
  const auto& caller_search = method_stats.find(caller_method);
  if (caller_search == method_stats.end()) {
    return false;
  }
  const auto& scores = m_min_scores.at(interaction_id);
  double warm_score = scores.first;
  double hot_score = scores.second;
  const auto& caller_stats = caller_search->second;
  auto caller_hits = caller_stats.call_count;
  auto caller_appears = caller_stats.appear_percent;
  if (caller_hits < warm_score || caller_appears < MIN_APPEAR_PERCENT) {
    return false;
  }

  const auto& callee_search = method_stats.find(callee_method);
  if (callee_search == method_stats.end()) {
    return false;
  }
  const auto& callee_stats = callee_search->second;
  auto callee_hits = callee_stats.call_count;
  auto callee_appears = callee_stats.appear_percent;
  if (callee_hits < warm_score || callee_appears < MIN_APPEAR_PERCENT) {
    return false;
  }

  // Smaller methods tend to benefit more from inlining. Allow warm + small
  // methods, or hot + medium size methods.
  constexpr uint32_t SMALL_ENOUGH = 20;
  bool either_small =
      caller_insns < SMALL_ENOUGH || callee_insns < SMALL_ENOUGH;
  bool either_hot = caller_hits >= hot_score || callee_hits >= hot_score;
  bool result = either_small || either_hot;
  if (result) {
    TRACE(METH_PROF,
          5,
          "%s, %s, %s, %u, %u, %f, %f",
          SHOW(caller_method),
          SHOW(callee_method),
          interaction_id.c_str(),
          caller_insns,
          callee_insns,
          caller_hits,
          callee_hits);
  }
  return result;
}

using namespace random_forest;

class InlineForSpeedDecisionTrees final : public InlineForSpeedBase {
 public:
  struct DecisionTreesConfig {
    boost::optional<float> min_method_hits = boost::none;
    boost::optional<float> min_method_appear = boost::none;
    boost::optional<float> min_block_hits = boost::none;
    boost::optional<float> min_block_appear = boost::none;
    boost::optional<std::vector<size_t>> interaction_indices = boost::none;
    boost::optional<size_t> exp_force_top_x_entries = boost::none;
    boost::optional<size_t> exp_force_top_x_entries_min_callee_size =
        boost::none;
    boost::optional<float> exp_force_top_x_entries_min_appear100 = boost::none;
    float accept_threshold{0};
    bool accept_over{true};
    bool break_chains{false};
  };

  InlineForSpeedDecisionTrees(const MethodProfiles* method_profiles,
                              PGIForest&& forest,
                              const DecisionTreesConfig& config)
      : m_method_context_context(method_profiles),
        m_forest(std::move(forest)),
        m_config(config) {
    if (m_config.exp_force_top_x_entries) {
      fetch_top_entries(method_profiles);
    }
  }

 protected:
  bool should_inline_impl(const DexMethod* caller_method,
                          const DexMethod* callee_method) override {
    auto& caller_context = get_or_create(caller_method);
    auto& callee_context = get_or_create(callee_method);

    float accepted{0};

    auto print_stats = [&](const char* suffix) {
      for (size_t i = 0;
           i != m_method_context_context.m_interaction_list.size();
           ++i) {

        auto get_val = [](const auto& vals, size_t i) {
          if (!vals) {
            return -1.0f;
          }
          if (!vals->hits[i]) {
            return -1.0f;
          }
          return *vals->hits[i];
        };
        auto caller_val = get_val(caller_context.m_vals, i);
        auto callee_val = get_val(callee_context.m_vals, i);

        auto get_appear_val = [](const auto& vals, size_t i) {
          if (!vals) {
            return -1.0f;
          }
          if (!vals->appear100[i]) {
            return -1.0f;
          }
          return *vals->appear100[i];
        };
        auto caller_appear_val = get_appear_val(caller_context.m_vals, i);
        auto callee_appear_val = get_appear_val(callee_context.m_vals, i);

        TRACE(METH_PROF,
              5,
              "[InlineForSpeedDecisionTrees%s] %.3f: "
              "%s!%u!%u!%u!%1.5f!%1.5f!%u!%u!%u!%u!%u!%s!%u!%u!%u!%1.5f!%1.5f!%"
              "u!%u!%u!%u!%u!%s",
              suffix,
              accepted,
              // Caller
              SHOW(caller_method),
              caller_context.m_params,
              caller_context.m_blocks,
              caller_context.m_edges,
              caller_val,
              caller_appear_val,
              caller_context.m_insns,
              caller_context.m_opcodes,
              caller_context.m_regs,
              caller_context.m_num_loops,
              caller_context.m_deepest_loop,
              // Callee
              SHOW(callee_method),
              callee_context.m_params,
              callee_context.m_blocks,
              callee_context.m_edges,
              callee_val,
              callee_appear_val,
              callee_context.m_insns,
              callee_context.m_opcodes,
              callee_context.m_regs,
              callee_context.m_num_loops,
              callee_context.m_deepest_loop,
              m_method_context_context.m_interaction_list[i].c_str());
      }
    };

    // While "normal" is more expensive, do it first anyways to fill `accepted`.
    if (!should_inline_normal(caller_method, callee_method, caller_context,
                              callee_context, accepted) &&
        !should_inline_exp(caller_method, callee_method, caller_context,
                           callee_context)) {
      if (traceEnabled(METH_PROF, 5)) {
        print_stats("-not");
      }
      return false;
    }

    if (traceEnabled(METH_PROF, 5)) {
      print_stats("");
    }

    if (m_config.break_chains) {
      m_inline_calls[caller_method].insert(callee_method);
    }

    return true;
  }

 private:
  template <typename Fn>
  bool test_any_interaction(const Fn& fn) const {
    if (!m_config.interaction_indices) {
      for (size_t i = 0;
           i != m_method_context_context.m_interaction_list.size();
           ++i) {
        if (fn(i)) {
          return true;
        }
      }
      return false;
    }

    for (size_t idx : *m_config.interaction_indices) {
      if (fn(idx)) {
        return true;
      }
    }

    return false;
  }

  bool should_inline_exp(const DexMethod* caller_method,
                         const DexMethod* callee_method,
                         const MethodContext& caller_context ATTRIBUTE_UNUSED,
                         const MethodContext& callee_context) {
    if (!m_config.exp_force_top_x_entries) {
      return false;
    }

    if (!test_any_interaction([&](size_t i) {
          return top_n_entries[i].count(caller_method) != 0 &&
                 top_n_entries[i].count(callee_method) != 0;
        })) {
      return false;
    }

    if (!m_config.exp_force_top_x_entries_min_callee_size) {
      return true;
    }
    if (*m_config.exp_force_top_x_entries_min_callee_size <=
        callee_context.m_insns) {
      return true;
    }

    return false;
  }

  bool should_inline_normal(const DexMethod* caller_method,
                            const DexMethod* callee_method,
                            const MethodContext& caller_context,
                            const MethodContext& callee_context,
                            float& accepted) {
    auto has_matching = [&](const auto& selector_fn, auto min_hits) {
      if (!caller_context.m_vals || !callee_context.m_vals) {
        return false;
      }

      return test_any_interaction([&](size_t idx) {
        const auto& caller_val = selector_fn(*caller_context.m_vals, idx);
        const auto& callee_val = selector_fn(*callee_context.m_vals, idx);
        return caller_val && *caller_val >= min_hits && callee_val &&
               *callee_val >= min_hits;
      });
    };

    // Explicitly check that the callee seems to ever be called with the caller.
    if (m_config.min_method_hits) {
      auto has_matching_hits =
          has_matching([](const auto& vals, size_t i) { return vals.hits[i]; },
                       *m_config.min_method_hits);
      if (!has_matching_hits) {
        TRACE(METH_PROF, 5, "%s calling %s: no samples together",
              SHOW(caller_method), SHOW(callee_method));
        return false;
      }
    }
    if (m_config.min_method_appear) {
      auto has_matching_appear = has_matching(
          [](const auto& vals, size_t i) { return vals.appear100[i]; },
          *m_config.min_method_appear);
      if (!has_matching_appear) {
        TRACE(METH_PROF, 5, "%s calling %s: no appear together",
              SHOW(caller_method), SHOW(callee_method));
        return false;
      }
    }

    bool default_ret =
        m_forest.accept(caller_context, callee_context, &accepted);
    if (m_config.accept_threshold == 0) {
      return default_ret;
    }
    return m_config.accept_over ? accepted >= m_config.accept_threshold
                                : accepted <= m_config.accept_threshold;
  }

 protected:
  bool should_inline_callsite_impl(const DexMethod* caller_method,
                                   const DexMethod* callee_method
                                       ATTRIBUTE_UNUSED,
                                   const cfg::Block* caller_block) final {
    // This is not really great, but it would mean recomputing the method-level
    // choice to understand.
    if (m_config.exp_force_top_x_entries) {
      return true;
    }

    auto compute_res = [&](const auto& threshold,
                           const auto& feature_fn) -> boost::optional<bool> {
      if (!threshold) {
        return boost::none;
      }
      auto min_hits = *threshold;
      auto sb_vec = source_blocks::gather_source_blocks(caller_block);
      if (sb_vec.empty()) {
        return false;
      }
      // Check all interactions.
      const auto* sb = sb_vec[0];

      return test_any_interaction([&](size_t i) {
        auto val = feature_fn(sb, i);
        return val && val >= min_hits;
      });
    };
    auto inline_hits =
        compute_res(m_config.min_block_hits,
                    [](const auto* sb, size_t i) { return sb->get_val(i); });
    if (inline_hits && !*inline_hits) {
      return false;
    }
    auto inline_appear =
        compute_res(m_config.min_block_appear, [](const auto* sb, size_t i) {
          return sb->get_appear100(i);
        });
    if (inline_appear && !*inline_appear) {
      return false;
    }

    if (m_config.break_chains) {
      {
        std::unique_lock<std::mutex> lock{m_inline_calls_mutex};
        if (!m_inline_calls_culled) {
          cull_inline_calls_now();
        }
      }
      if (m_inline_calls.count(caller_method) == 0) {
        return false;
      }
    }

    return true;
  }

 private:
  const MethodContext& get_or_create(const DexMethod* m) {
    auto it = m_cache.find(m);
    if (it != m_cache.end()) {
      return it->second;
    }

    auto insert_it = m_cache.emplace(m, m_method_context_context.create(m));
    return insert_it.first->second;
  }

  void fetch_top_entries(const MethodProfiles* method_profiles) {
    const auto& interactions = g_redex->get_sb_interaction_indices();
    top_n_entries.resize(interactions.size());

    const auto& all = method_profiles->all_interactions();
    for (const auto& p : interactions) {
      auto it = all.find(p.first);
      redex_assert(it != all.end());
      const auto& stats_map = it->second;
      std::vector<std::pair<const DexMethodRef*, double>> tmp_vec;
      tmp_vec.reserve(stats_map.size());

      boost::transform(
          stats_map | boost::adaptors::filtered([&](const auto& p) {
            return !m_config.exp_force_top_x_entries_min_appear100 ||
                   p.second.appear_percent >=
                       *m_config.exp_force_top_x_entries_min_appear100;
          }),
          std::back_inserter(tmp_vec), [](const auto& p) {
            return std::make_pair(p.first, p.second.call_count);
          });
      std::sort(tmp_vec.begin(), tmp_vec.end(),
                [](const auto& lhs, const auto& rhs) {
                  return lhs.second > rhs.second;
                });
      if (tmp_vec.size() > *m_config.exp_force_top_x_entries) {
        tmp_vec.resize(*m_config.exp_force_top_x_entries);
      }
      std::transform(tmp_vec.begin(), tmp_vec.end(),
                     std::inserter(top_n_entries.at(p.second),
                                   top_n_entries.at(p.second).begin()),
                     [](const auto& p) { return p.first; });
    }
  }

  void cull_inline_calls_now() {
    // This is a simplistic greedy algorithm. We take the highest edge, as given
    // by the very simple heuristic of callee call-count (then name). We could
    // probably scale by caller call-count, or appear.

    auto edge_heuristic = [&](const auto* /*caller*/,
                              const auto* callee) -> float {
      const auto& vals = m_cache.at(callee).m_vals;
      if (!vals) {
        return -1;
      }
      const auto& val = vals->hits.at(0);
      if (!val) {
        return 0;
      }
      return *val;
    };

    auto order = [&](const auto& lhs, const auto& rhs) {
      auto lhs_h = edge_heuristic(lhs.first, lhs.second);
      auto rhs_h = edge_heuristic(rhs.first, rhs.second);
      if (lhs_h != rhs_h) {
        return lhs_h > rhs_h;
      }
      if (lhs.first != rhs.first) {
        return compare_dexmethods(lhs.first, rhs.first);
      }
      return compare_dexmethods(lhs.second, rhs.second);
    };

    using ElemT = std::pair<const DexMethod*, const DexMethod*>;
    std::priority_queue<ElemT, std::vector<ElemT>, decltype(order)> queue(
        order);

    auto filtered_map = m_inline_calls;

    // Fill the queue with all our edges.
    for (const auto& p : m_inline_calls) {
      for (const auto* callee : p.second) {
        if (p.first != callee) { // No cycles.
          queue.emplace(p.first, callee);
          filtered_map[callee];
        } else {
          filtered_map.at(p.first).erase(callee);
        }
      }
    }

    while (!queue.empty()) {
      auto top = queue.top();
      queue.pop();

      auto* caller = top.first;
      auto* callee = top.second;

      if (filtered_map.at(caller).count(callee) == 0) {
        continue;
      }

      // Remove all out edges from the callee.
      filtered_map.at(callee).clear();
    }

    {
      for (auto it = filtered_map.begin(); it != filtered_map.end();) {
        if (it->second.empty()) {
          it = filtered_map.erase(it);
        } else {
          ++it;
        }
      }
    }

    m_inline_calls = std::move(filtered_map);

    m_inline_calls_culled = true;
  }

  MethodContextContext m_method_context_context;
  std::unordered_map<const DexMethod*, MethodContext> m_cache;
  PGIForest m_forest;
  DecisionTreesConfig m_config;
  std::vector<std::unordered_set<const DexMethodRef*>> top_n_entries;
  // Collect "yes" decisions based on methods, possibly to break chains later.
  std::mutex m_inline_calls_mutex;
  std::unordered_map<const DexMethod*, std::unordered_set<const DexMethod*>>
      m_inline_calls;
  bool m_inline_calls_culled{false};
};

class InlineForSpeedCallerList final : public InlineForSpeedBase {
 public:
  explicit InlineForSpeedCallerList(const std::vector<std::string>& caller_list,
                                    bool by_prefix,
                                    const MethodProfiles* method_profiles,
                                    float callee_min_hits,
                                    float callee_min_appear)
      : m_caller_methods(gather_methods(caller_list, by_prefix)),
        m_method_profiles(method_profiles),
        m_callee_min_hits(callee_min_hits),
        m_callee_min_appear(callee_min_appear),
        m_method_context_context(method_profiles) {}

 protected:
  bool should_inline_impl(const DexMethod* caller_method,
                          const DexMethod* callee_method) final {
    if (m_caller_methods.count(caller_method) == 0) {
      return false;
    }

    // If the pair is hot under any interaction, inline it.
    auto do_inline = [&]() {
      for (const auto& pair : m_method_profiles->all_interactions()) {
        auto it = pair.second.find(callee_method);
        if (it == pair.second.end()) {
          continue;
        }

        if (it->second.call_count >= m_callee_min_hits &&
            it->second.appear_percent >= m_callee_min_appear) {
          return true;
        }
      }
      return false;
    }();

    if (!do_inline) {
      return false;
    }

    if (traceEnabled(METH_PROF, 5)) {
      const auto& caller_context = get_or_create(caller_method);
      const auto& callee_context = get_or_create(callee_method);

      for (size_t i = 0;
           i != m_method_context_context.m_interaction_list.size();
           ++i) {

        auto get_val = [](const auto& vals, size_t i) {
          if (!vals) {
            return -1.0f;
          }
          if (!vals->hits[i]) {
            return -1.0f;
          }
          return *vals->hits[i];
        };
        auto caller_val = get_val(caller_context.m_vals, i);
        auto callee_val = get_val(callee_context.m_vals, i);

        auto get_appear_val = [](const auto& vals, size_t i) {
          if (!vals) {
            return -1.0f;
          }
          if (!vals->appear100[i]) {
            return -1.0f;
          }
          return *vals->appear100[i];
        };
        auto caller_appear_val = get_appear_val(caller_context.m_vals, i);
        auto callee_appear_val = get_appear_val(callee_context.m_vals, i);

        TRACE(METH_PROF,
              5,
              "[InlineForSpeedDecisionTrees] %zu: "
              "%s!%u!%u!%u!%1.5f!%1.5f!%u!%u!%u!%u!%u!%s!%u!%u!%u!%1.5f!%1.5f!%"
              "u!%u!%u!%u!%u!%s",
              (size_t)0,
              // Caller
              SHOW(caller_method),
              caller_context.m_params,
              caller_context.m_blocks,
              caller_context.m_edges,
              caller_val,
              caller_appear_val,
              caller_context.m_insns,
              caller_context.m_opcodes,
              caller_context.m_regs,
              caller_context.m_num_loops,
              caller_context.m_deepest_loop,
              // Callee
              SHOW(callee_method),
              callee_context.m_params,
              callee_context.m_blocks,
              callee_context.m_edges,
              callee_val,
              callee_appear_val,
              callee_context.m_insns,
              callee_context.m_opcodes,
              callee_context.m_regs,
              callee_context.m_num_loops,
              callee_context.m_deepest_loop,
              m_method_context_context.m_interaction_list[i].c_str());
      }
    }

    return true;
  }

  bool should_inline_callsite_impl(
      const DexMethod* caller_method ATTRIBUTE_UNUSED,
      const DexMethod* callee_method ATTRIBUTE_UNUSED,
      const cfg::Block* caller_block ATTRIBUTE_UNUSED) final {
    // TODO: Maybe do this?
    return true;
  }

 private:
  // Binding late to support methods synthesized before PGI time.
  static std::unordered_set<const DexMethodRef*> gather_methods(
      const std::vector<std::string>& caller_list, bool by_prefix) {
    std::unordered_set<const DexMethodRef*> ret;
    auto collect = [&](auto fn) {
      for (const auto& str_mref : caller_list) {
        auto* mref = fn(str_mref);
        if (mref != nullptr) {
          ret.insert(mref);
        } else {
          std::cerr << "Warning: Could not find " << str_mref << std::endl;
        }
      }
    };

    if (by_prefix) {
      auto prefix_fn = [](auto& str) -> const DexMethodRef* {
        // Array class?
        {
          auto bracket_pos = str.find('[');
          if (bracket_pos != std::string::npos) {
            return nullptr;
          }
        }

        auto pos = str.rfind('.');
        if (pos == std::string::npos || pos == 0 || pos == str.size() - 1) {
          return nullptr;
        }

        DexClass* cls;
        {
          auto external_class_name = str.substr(0, pos);
          auto internal_class_name =
              java_names::external_to_internal(external_class_name);

          auto type = DexType::get_type(internal_class_name);
          if (type == nullptr) {
            return nullptr;
          }
          cls = type_class(type);
          if (cls == nullptr) {
            return nullptr;
          }
          if (cls->is_external()) {
            return nullptr;
          }
        }

        // OK, seem to have a good class, now look for the method.
        auto method_name = str.substr(pos + 1);
        boost::optional<const DexMethod*> found{boost::none};
        for (auto* m : cls->get_all_methods()) {
          if (m->get_name()->str() == method_name) {
            if (found) {
              std::cerr << "Ambiguous method " << method_name << std::endl;
              found = boost::none;
              break;
            }
            found = m;
          }
        }
        if (found) {
          return *found;
        }
        return nullptr;
      };
      collect(prefix_fn);
    } else {
      collect([](auto& str) { return DexMethod::get_method(str); });
    }

    return ret;
  }

  const MethodContext& get_or_create(const DexMethod* m) {
    auto it = m_cache.find(m);
    if (it != m_cache.end()) {
      return it->second;
    }

    auto insert_it = m_cache.emplace(m, m_method_context_context.create(m));
    return insert_it.first->second;
  }

  const std::unordered_set<const DexMethodRef*> m_caller_methods;
  const MethodProfiles* m_method_profiles;
  const float m_callee_min_hits;
  const float m_callee_min_appear;

  // For TRACE.
  MethodContextContext m_method_context_context;
  std::unordered_map<const DexMethod*, MethodContext> m_cache;
};

enum class IFSMode {
  kMethodProfiles,
  kForest,
  kCallerList,
};

} // namespace

PerfMethodInlinePass::PerfMethodInlinePass() : Pass("PerfMethodInlinePass") {}

PerfMethodInlinePass::~PerfMethodInlinePass() {}

struct PerfMethodInlinePass::Config {
  boost::optional<random_forest::PGIForest> forest = boost::none;
  InlineForSpeedDecisionTrees::DecisionTreesConfig dec_trees_config;
  std::string interactions_str;
  boost::optional<std::vector<std::string>> caller_list = boost::none;
  bool caller_list_prefix{false};
  float caller_list_callee_min_hits{0};
  float caller_list_callee_min_appear{0};
  IFSMode ifs{IFSMode::kMethodProfiles};

  boost::optional<std::vector<size_t>> get_interactions(
      const RedexContext& ctx) {
    if (interactions_str.empty()) {
      return boost::none;
    }
    const auto& map = ctx.get_sb_interaction_indices();
    std::vector<std::string> split;
    boost::split(split, interactions_str, boost::is_any_of(","));
    std::vector<size_t> indices;
    indices.reserve(split.size());
    for (const auto& str : split) {
      auto it = map.find(str);
      always_assert_log(it != map.end(), "%s not found!", str.c_str());
      indices.push_back(it->second);
    }

    return indices;
  }
};

void PerfMethodInlinePass::bind_config() {
  std::string random_forest_file;
  bind("random_forest_file", "", random_forest_file);
  float accept_threshold;
  bind("accept_threshold", 0, accept_threshold,
       "Threshold of trees to accept an inlining decision. 0 uses default "
       "(half).");
  bool accept_over;
  bind("accept_over", true, accept_over,
       "Comparison is accept-value >= threshold when true and accept-value <= "
       "threshold otherwise");
  float min_hits;
  bind("min_hits", std::numeric_limits<float>::min(), min_hits,
       "Threshold for caller and callee method call-count to consider "
       "inlining. A negative value elides the check.");
  float min_appear;
  bind("min_appear", 1.0f, min_appear,
       "Threshold for caller and callee method appear100 to consider "
       "inlining. A negative value elides the check.");
  float min_block_hits;
  bind("min_block_hits", -1.0f, min_block_hits,
       "Threshold for caller source-block value to consider inlining. A "
       "negative value elides the check.");
  float min_block_appear;
  bind("min_block_appear", -1.0f, min_block_appear,
       "Threshold for caller source-block appear100 to consider inlining. A "
       "negative value elides the check.");
  bool break_chains;
  bind("break_chains", true, break_chains);
  std::string interactions_str;
  bind("interactions", "", interactions_str,
       "Comma-separated list of interactions to use. An empty value uses all "
       "interactions.");
  size_t exp_force_top_x_entries;
  bind("exp_force_top_x_entries", 0, exp_force_top_x_entries,
       "For experiments: If greater than zero, always accept caller/callee "
       "pairs that are in the top N of the profile.");
  size_t exp_force_top_x_entries_min_callee_size;
  bind("exp_force_top_x_entries_min_callee_size", 0,
       exp_force_top_x_entries_min_callee_size,
       "For experiments: If greater than zero, restrict always-accept "
       "caller/callee pairs from exp_force_top_x_entries to callees of at "
       "least the given size (in instructions)");
  float exp_force_top_x_entries_min_appear100;
  bind("exp_force_top_x_entries_min_appear100", -1.0f,
       exp_force_top_x_entries_min_appear100,
       "For experiments: If non-negative, restrict always-accept caller/callee "
       "pairs from exp_force_top_x_entries to callers and callees that appear "
       "at least this amount.");
  std::string caller_list_file;
  bind("caller_list_file", "", caller_list_file);
  bool caller_list_prefix;
  bind("caller_list_prefix", false, caller_list_prefix);
  float caller_list_callee_min_hits;
  bind("caller_list_callee_min_hits", 1.0f, caller_list_callee_min_hits);
  float caller_list_callee_min_appear;
  bind("caller_list_callee_min_appear", 1.0f, caller_list_callee_min_appear);
  std::string which_ifs;
  bind("decision_mode", "", which_ifs);
  after_configuration([this, random_forest_file, accept_threshold, accept_over,
                       min_hits, min_appear, min_block_hits, min_block_appear,
                       interactions_str, exp_force_top_x_entries,
                       exp_force_top_x_entries_min_callee_size,
                       exp_force_top_x_entries_min_appear100, caller_list_file,
                       caller_list_prefix, caller_list_callee_min_hits,
                       caller_list_callee_min_appear, which_ifs,
                       break_chains]() {
    this->m_config = std::make_unique<PerfMethodInlinePass::Config>();
    if (!random_forest_file.empty()) {
      std::stringstream buffer;
      {
        std::ifstream ifs(random_forest_file);
        always_assert(ifs);
        buffer << ifs.rdbuf();
        always_assert(!ifs.fail());
      }
      // For simplicity, accept an empty file.
      if (!buffer.str().empty()) {
        this->m_config->forest = random_forest::PGIForest::deserialize(
            buffer.str(), random_forest::get_default_feature_function_map());
        TRACE(METH_PROF, 1, "Loaded a forest with %zu decision trees.",
              this->m_config->forest->size());
      }
    }
    auto assign_opt = [](float v) -> boost::optional<float> {
      return v < 0 ? boost::none : boost::optional<float>(v);
    };
    auto& dec_trees_config = this->m_config->dec_trees_config;
    dec_trees_config.accept_threshold = accept_threshold;
    dec_trees_config.accept_over = accept_over;
    dec_trees_config.min_method_hits = assign_opt(min_hits);
    dec_trees_config.min_method_appear = assign_opt(min_appear);
    dec_trees_config.min_block_hits = assign_opt(min_block_hits);
    dec_trees_config.min_block_appear = assign_opt(min_block_appear);
    dec_trees_config.break_chains = break_chains;
    this->m_config->interactions_str = interactions_str;

    auto assign_opt_size_t = [](size_t v) -> boost::optional<size_t> {
      return v == 0 ? boost::none : boost::optional<size_t>(v);
    };
    dec_trees_config.exp_force_top_x_entries =
        assign_opt_size_t(exp_force_top_x_entries);
    dec_trees_config.exp_force_top_x_entries_min_callee_size =
        assign_opt_size_t(exp_force_top_x_entries_min_callee_size);
    dec_trees_config.exp_force_top_x_entries_min_appear100 =
        assign_opt(exp_force_top_x_entries_min_appear100);

    if (!caller_list_file.empty()) {
      std::ifstream ifs(caller_list_file);
      always_assert(ifs);
      std::string tmp;
      std::vector<std::string> str_vec;
      while (std::getline(ifs, tmp)) {
        str_vec.emplace_back(std::move(tmp));
      }
      always_assert(!ifs.bad());
      this->m_config->caller_list = std::move(str_vec);
    }
    this->m_config->caller_list_callee_min_hits = caller_list_callee_min_hits;
    this->m_config->caller_list_callee_min_appear =
        caller_list_callee_min_appear;
    this->m_config->caller_list_prefix = caller_list_prefix;

    if (!which_ifs.empty()) {
      if (which_ifs == "caller-list") {
        redex_assert(this->m_config->caller_list);
        this->m_config->ifs = IFSMode::kCallerList;
      } else if (which_ifs == "forest") {
        redex_assert(this->m_config->forest);
        this->m_config->ifs = IFSMode::kForest;
      } else {
        redex_assert(which_ifs == "method-profiles");
        this->m_config->ifs = IFSMode::kMethodProfiles;
      }
    } else {
      // Prefer forest.
      if (this->m_config->forest) {
        this->m_config->ifs = IFSMode::kForest;
      } else if (this->m_config->caller_list) {
        this->m_config->ifs = IFSMode::kCallerList;
      } else {
        this->m_config->ifs = IFSMode::kMethodProfiles;
      }
    }
  });
}

void PerfMethodInlinePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(METH_PROF,
          1,
          "Skipping PerfMethodInlinePass because Instrumentation is enabled");
    return;
  }

  redex_assert(m_config);

  const auto& method_profiles = conf.get_method_profiles();
  if (!method_profiles.has_stats()) {
    // PerfMethodInline is enabled, but there are no profiles available. Bail,
    // don't run a regular inline pass.
    TRACE(METH_PROF, 1, "No profiling data available");
    return;
  }

  // Unique pointer for indirection and single path.
  std::unique_ptr<InlineForSpeedBase> ifs{[&]() -> InlineForSpeedBase* {
    switch (m_config->ifs) {
    case IFSMode::kForest: {
      redex_assert(m_config->forest);
      if (m_config->dec_trees_config.exp_force_top_x_entries) {
        mgr.set_metric("exp_force_top_x_entries",
                       *m_config->dec_trees_config.exp_force_top_x_entries);
        if (m_config->dec_trees_config
                .exp_force_top_x_entries_min_callee_size) {
          mgr.set_metric("exp_force_top_x_entries_min_callee_size",
                         *m_config->dec_trees_config
                              .exp_force_top_x_entries_min_callee_size);
        }
      }

      m_config->dec_trees_config.interaction_indices =
          m_config->get_interactions(*g_redex);
      return new InlineForSpeedDecisionTrees(&method_profiles,
                                             m_config->forest->clone(),
                                             m_config->dec_trees_config);
    }

    case IFSMode::kCallerList: {
      redex_assert(m_config->caller_list);
      mgr.set_metric("caller_list_size", m_config->caller_list->size());
      mgr.set_metric("caller_list_callee_min_hits_100",
                     m_config->caller_list_callee_min_hits * 100);
      mgr.set_metric("caller_list_callee_min_appear_100",
                     m_config->caller_list_callee_min_appear * 100);
      return new InlineForSpeedCallerList(
          *m_config->caller_list,
          m_config->caller_list_prefix,
          &method_profiles,
          m_config->caller_list_callee_min_hits,
          m_config->caller_list_callee_min_appear);
    }

    case IFSMode::kMethodProfiles:
      return new InlineForSpeedMethodProfiles(&method_profiles);
    }
    not_reached();
  }()};

  inliner::run_inliner(stores, mgr, conf, /* intra_dex */ true,
                       /* inline_for_speed= */ ifs.get());

  TRACE(METH_PROF, 1, "Accepted %zu out of %zu choices.",
        ifs->get_num_accepted(), ifs->get_num_choices());
  mgr.set_metric("pgi_inline_choices", ifs->get_num_choices());
  mgr.set_metric("pgi_inline_choices_accepted", ifs->get_num_accepted());
  mgr.set_metric("pgi_inline_callsite_choices",
                 ifs->get_num_callsite_choices());
  mgr.set_metric("pgi_inline_callsite_choices_accepted",
                 ifs->get_num_callsite_accepted());
  mgr.set_metric("pgi_use_random_forest", m_config->forest ? 1 : 0);
  mgr.set_metric("pgi_use_caller_list", m_config->forest        ? 0
                                        : m_config->caller_list ? 1
                                                                : 0);
  if (m_config->forest) {
    auto opt = m_config->get_interactions(*g_redex);
    mgr.set_metric("pgi_interactions",
                   opt ? opt->size()
                       : g_redex->get_sb_interaction_indices().size());
  }
}

static PerfMethodInlinePass s_pass;
