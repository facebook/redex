/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PerfMethodInlinePass.h"

#include <queue>

#include "ConfigFiles.h"
#include "IRCode.h"
#include "InlineForSpeed.h"
#include "MethodInliner.h"
#include "MethodProfiles.h"
#include "PGIForest.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"

namespace {

using namespace method_profiles;

class InlineForSpeedBase : public InlineForSpeed {
 public:
  bool should_inline(const DexMethod* caller_method,
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

 protected:
  virtual bool should_inline_impl(const DexMethod* caller_method,
                                  const DexMethod* callee_method) = 0;

  size_t m_num_choices{0};
  size_t m_num_accepted{0};
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
  InlineForSpeedDecisionTrees(const MethodProfiles* method_profiles,
                              PGIForest&& forest)
      : m_method_context_context(method_profiles),
        m_forest(std::move(forest)) {}

 protected:
  bool should_inline_impl(const DexMethod* caller_method,
                          const DexMethod* callee_method) override {
    auto& caller_context = get_or_create(caller_method);
    auto& callee_context = get_or_create(callee_method);

    size_t accepted;
    if (!m_forest.accept(caller_context, callee_context, &accepted)) {
      return false;
    }
    auto get_max_float = [](const std::vector<boost::optional<float>>& v) {
      float max = -1;
      for (const auto& opt : v) {
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
          get_max_float(caller_context.m_hits),
          caller_context.m_insns,
          caller_context.m_regs,
          caller_context.m_num_loops,
          caller_context.m_deepest_loop,
          // Callee
          SHOW(callee_method),
          callee_context.m_blocks,
          callee_context.m_edges,
          get_max_float(callee_context.m_hits),
          callee_context.m_insns,
          callee_context.m_regs,
          callee_context.m_num_loops,
          callee_context.m_deepest_loop);
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
};

} // namespace

PerfMethodInlinePass::~PerfMethodInlinePass() {}

struct PerfMethodInlinePass::Config {
  boost::optional<random_forest::PGIForest> forest = boost::none;
};

void PerfMethodInlinePass::bind_config() {
  std::string random_forest_file;
  bind("random_forest_file", "", random_forest_file);
  after_configuration([this, random_forest_file]() {
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
      m_config->forest ? (InlineForSpeedBase*)(new InlineForSpeedDecisionTrees(
                             &method_profiles, m_config->forest->clone()))
                       : new InlineForSpeedMethodProfiles(&method_profiles)};

  inliner::run_inliner(stores, mgr, conf, /* intra_dex */ true,
                       /* inline_for_speed= */ ifs.get());

  TRACE(METH_PROF, 1, "Accepted %zu out of %zu choices.",
        ifs->get_num_accepted(), ifs->get_num_choices());
  mgr.set_metric("pgi_inline_choices", ifs->get_num_choices());
  mgr.set_metric("pgi_inline_choices_accepted", ifs->get_num_accepted());
  mgr.set_metric("pgi_use_random_forest", m_config->forest ? 1 : 0);
}

static PerfMethodInlinePass s_pass;
