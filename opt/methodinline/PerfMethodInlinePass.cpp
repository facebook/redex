/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PerfMethodInlinePass.h"

#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <limits>
#include <queue>

#include "ConfigFiles.h"
#include "ControlFlow.h"
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
  InlineForSpeedDecisionTrees(
      const MethodProfiles* method_profiles,
      PGIForest&& forest,
      const boost::optional<float>& min_method_hits,
      const boost::optional<float>& min_method_appear,
      const boost::optional<float>& min_block_hits,
      const boost::optional<float>& min_block_appear,
      const boost::optional<std::vector<size_t>>& interactions)
      : m_method_context_context(method_profiles),
        m_forest(std::move(forest)),
        m_min_method_hits(min_method_hits),
        m_min_method_appear(min_method_appear),
        m_min_block_hits(min_block_hits),
        m_min_block_appear(min_block_appear),
        m_interaction_indices(interactions) {}

 protected:
  bool should_inline_impl(const DexMethod* caller_method,
                          const DexMethod* callee_method) override {
    auto& caller_context = get_or_create(caller_method);
    auto& callee_context = get_or_create(callee_method);

    auto has_matching = [&](const auto& selector_fn, auto min_hits) {
      if (!caller_context.m_vals || !callee_context.m_vals) {
        return false;
      }

      auto check_interaction = [&](size_t idx) {
        const auto& caller_val = selector_fn(*caller_context.m_vals, idx);
        const auto& callee_val = selector_fn(*callee_context.m_vals, idx);
        return caller_val && *caller_val >= min_hits && callee_val &&
               *callee_val >= min_hits;
      };

      if (!m_interaction_indices) {
        for (size_t i = 0;
             i != m_method_context_context.m_interaction_list.size();
             ++i) {
          if (check_interaction(i)) {
            return true;
          }
        }
        return false;
      }

      for (size_t idx : *m_interaction_indices) {
        if (check_interaction(idx)) {
          return true;
        }
      }
      return false;
    };

    // Explicitly check that the callee seems to ever be called with the caller.
    if (m_min_method_hits) {
      auto has_matching_hits =
          has_matching([](const auto& vals, size_t i) { return vals.hits[i]; },
                       *m_min_method_hits);
      if (!has_matching_hits) {
        TRACE(METH_PROF, 5, "%s calling %s: no samples together",
              SHOW(caller_method), SHOW(callee_method));
        return false;
      }
    }
    if (m_min_method_appear) {
      auto has_matching_appear = has_matching(
          [](const auto& vals, size_t i) { return vals.appear100[i]; },
          *m_min_method_appear);
      if (!has_matching_appear) {
        TRACE(METH_PROF, 5, "%s calling %s: no appear together",
              SHOW(caller_method), SHOW(callee_method));
        return false;
      }
    }

    size_t accepted;
    if (!m_forest.accept(caller_context, callee_context, &accepted)) {
      return false;
    }
    auto get_max_hit_float = [](const auto& vals) {
      if (!vals) {
        return -1.0f;
      }
      float max = -1;
      for (const auto& opt : vals->hits) {
        if (opt) {
          max = std::max(max, *opt);
        }
      }
      return max;
    };
    TRACE(METH_PROF,
          5,
          "[InlineForSpeedDecisionTrees] "
          "%zu: %s!%u!%u!%1.5f!%u!%u!%u!%u!%s!%u!%u!%1.5f!%u!%u!%u!%u",
          accepted,
          // Caller
          SHOW(caller_method),
          caller_context.m_blocks,
          caller_context.m_edges,
          get_max_hit_float(caller_context.m_vals),
          caller_context.m_insns,
          caller_context.m_regs,
          caller_context.m_num_loops,
          caller_context.m_deepest_loop,
          // Callee
          SHOW(callee_method),
          callee_context.m_blocks,
          callee_context.m_edges,
          get_max_hit_float(callee_context.m_vals),
          callee_context.m_insns,
          callee_context.m_regs,
          callee_context.m_num_loops,
          callee_context.m_deepest_loop);
    return true;
  }

  bool should_inline_callsite_impl(
      const DexMethod* caller_method ATTRIBUTE_UNUSED,
      const DexMethod* callee_method ATTRIBUTE_UNUSED,
      const cfg::Block* caller_block) final {
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

      if (!m_interaction_indices) {
        size_t num = g_redex->num_sb_interaction_indices();
        for (size_t i = 0; i != num; ++i) {
          auto val = feature_fn(sb, i);
          if (val && val >= min_hits) {
            return true;
          }
        }
        return false;
      }

      for (size_t idx : *m_interaction_indices) {
        auto val = feature_fn(sb, idx);
        if (val && val >= min_hits) {
          return true;
        }
      }
      return false;
    };
    auto inline_hits =
        compute_res(m_min_block_hits,
                    [](const auto* sb, size_t i) { return sb->get_val(i); });
    if (inline_hits && !*inline_hits) {
      return false;
    }
    auto inline_appear =
        compute_res(m_min_block_appear, [](const auto* sb, size_t i) {
          return sb->get_appear100(i);
        });
    if (inline_appear && !*inline_appear) {
      return false;
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

  MethodContextContext m_method_context_context;
  std::unordered_map<const DexMethod*, MethodContext> m_cache;
  PGIForest m_forest;
  boost::optional<float> m_min_method_hits;
  boost::optional<float> m_min_method_appear;
  boost::optional<float> m_min_block_hits;
  boost::optional<float> m_min_block_appear;
  boost::optional<std::vector<size_t>> m_interaction_indices;
};

} // namespace

PerfMethodInlinePass::~PerfMethodInlinePass() {}

struct PerfMethodInlinePass::Config {
  boost::optional<random_forest::PGIForest> forest = boost::none;
  boost::optional<float> min_method_hits = std::numeric_limits<float>::min();
  boost::optional<float> min_method_appear = 1.0f; // 1% cutoff
  boost::optional<float> min_block_hits = boost::none;
  boost::optional<float> min_block_appear = boost::none;
  std::string interactions_str;

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
  std::string interactions_str;
  bind("interactions", "", interactions_str,
       "Comma-separated list of interactions to use. An empty value uses all "
       "interactions.");
  after_configuration([this, random_forest_file, min_hits, min_appear,
                       min_block_hits, min_block_appear, interactions_str]() {
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
    this->m_config->min_method_hits = assign_opt(min_hits);
    this->m_config->min_method_appear = assign_opt(min_appear);
    this->m_config->min_block_hits = assign_opt(min_block_hits);
    this->m_config->min_block_appear = assign_opt(min_block_appear);
    this->m_config->interactions_str = interactions_str;
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
  std::unique_ptr<InlineForSpeedBase> ifs{
      m_config->forest
          ? (InlineForSpeedBase*)(new InlineForSpeedDecisionTrees(
                &method_profiles, m_config->forest->clone(),
                m_config->min_method_hits, m_config->min_method_appear,
                m_config->min_block_hits, m_config->min_block_appear,
                m_config->get_interactions(*g_redex)))
          : new InlineForSpeedMethodProfiles(&method_profiles)};

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
  if (m_config->forest) {
    auto opt = m_config->get_interactions(*g_redex);
    mgr.set_metric("pgi_interactions",
                   opt ? opt->size()
                       : g_redex->get_sb_interaction_indices().size());
  }
}

static PerfMethodInlinePass s_pass;
