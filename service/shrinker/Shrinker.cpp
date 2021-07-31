/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Shrinker.h"

#include "ConstructorParams.h"
#include "LinearScan.h"
#include "RandomForest.h"
#include "RegisterAllocation.h"
#include "ScopedMetrics.h"
#include "Trace.h"

namespace shrinker {

namespace {

inline Shrinker::ShrinkerForest::FeatureFunctionMap
get_default_feature_function_map() {
  return {
      // Caller.
      {"caller_insns",
       [](const Shrinker::MethodContext& caller) { return caller.m_insns; }},
      {"caller_regs",
       [](const Shrinker::MethodContext& caller) { return caller.m_regs; }},
      {"caller_blocks",
       [](const Shrinker::MethodContext& caller) { return caller.m_blocks; }},
      {"caller_edges",
       [](const Shrinker::MethodContext& caller) { return caller.m_edges; }},
  };
}

Shrinker::ShrinkerForest load(const std::string& filename) {
  std::string content;
  if (!filename.empty() && filename != "none") {
    std::stringstream buffer;
    {
      std::ifstream ifs(filename);
      always_assert(ifs);
      buffer << ifs.rdbuf();
      always_assert(!ifs.fail());
    }
    content = buffer.str();
  }
  if (content.empty()) {
    TRACE(MMINL, 1, "No shrinker forest: %s", filename.c_str());
    return Shrinker::ShrinkerForest(); // Empty forest accepts everything.
  }
  return Shrinker::ShrinkerForest::deserialize(
      content, get_default_feature_function_map());
}

bool should_shrink(IRCode* code, const Shrinker::ShrinkerForest& forest) {
  if (!code->editable_cfg_built()) {
    code->build_cfg(/* editable= */ true);
  }
  const auto& cfg = code->cfg();

  size_t regs = cfg.get_registers_size();
  size_t insn = cfg.num_opcodes();
  size_t blocks = cfg.num_blocks();
  size_t edges = cfg.num_edges();

  return forest.accept(Shrinker::MethodContext{
      (uint32_t)regs, (uint32_t)insn, (uint32_t)blocks, (uint32_t)edges});
}

} // namespace

Shrinker::Shrinker(
    DexStoresVector& stores,
    const Scope& scope,
    const ShrinkerConfig& config,
    const std::unordered_set<DexMethodRef*>& configured_pure_methods,
    const std::unordered_set<DexString*>& configured_finalish_field_names)
    : m_forest(load(config.reg_alloc_random_forest)),
      m_xstores(stores),
      m_config(config),
      m_enabled(config.run_const_prop || config.run_cse ||
                config.run_copy_prop || config.run_local_dce ||
                config.run_reg_alloc || config.run_fast_reg_alloc ||
                config.run_dedup_blocks),
      m_pure_methods(configured_pure_methods),
      m_finalish_field_names(configured_finalish_field_names) {
  if (config.run_cse || config.run_local_dce) {
    if (config.compute_pure_methods) {
      const auto& pure_methods = get_pure_methods();
      m_pure_methods.insert(pure_methods.begin(), pure_methods.end());
      auto immutable_getters = get_immutable_getters(scope);
      m_pure_methods.insert(immutable_getters.begin(), immutable_getters.end());
    }
    if (config.run_cse) {
      m_cse_shared_state = std::make_unique<cse_impl::SharedState>(
          m_pure_methods, m_finalish_field_names);
    }
    if (config.run_local_dce && config.compute_pure_methods) {
      std::unique_ptr<const method_override_graph::Graph> owned_override_graph;
      const method_override_graph::Graph* override_graph;
      if (config.run_cse) {
        override_graph = m_cse_shared_state->get_method_override_graph();
      } else {
        owned_override_graph = method_override_graph::build_graph(scope);
        override_graph = owned_override_graph.get();
      }
      std::unordered_set<const DexMethod*> computed_no_side_effects_methods;
      /* Returns computed_no_side_effects_methods_iterations */
      compute_no_side_effects_methods(scope, override_graph, m_pure_methods,
                                      &computed_no_side_effects_methods);
      for (auto m : computed_no_side_effects_methods) {
        m_pure_methods.insert(const_cast<DexMethod*>(m));
      }
    }
  }
  if (config.run_const_prop && config.analyze_constructors) {
    constant_propagation::immutable_state::analyze_constructors(
        scope, &m_immut_analyzer_state);
  }
}

void Shrinker::shrink_method(DexMethod* method) {
  auto code = method->get_code();
  bool editable_cfg_built = code->editable_cfg_built();

  constant_propagation::Transform::Stats const_prop_stats;
  cse_impl::Stats cse_stats;
  copy_propagation_impl::Stats copy_prop_stats;
  LocalDce::Stats local_dce_stats;
  dedup_blocks_impl::Stats dedup_blocks_stats;

  if (m_config.run_const_prop) {
    auto timer = m_const_prop_timer.scope();
    if (editable_cfg_built) {
      code->clear_cfg();
    }
    if (!code->cfg_built()) {
      code->build_cfg(/* editable */ false);
    }
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(),
          constant_propagation::ConstantPrimitiveAndBoxedAnalyzer(
              &m_immut_analyzer_state, &m_immut_analyzer_state,
              constant_propagation::EnumFieldAnalyzerState::get(),
              constant_propagation::BoxedBooleanAnalyzerState::get(), nullptr),
          /* imprecise_switches */ true);
      fp_iter.run({});
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      const_prop_stats = tf.apply_on_uneditable_cfg(
          fp_iter, constant_propagation::WholeProgramState(), code, &m_xstores,
          method->get_class());
    }
    always_assert(!code->editable_cfg_built());
    code->build_cfg(/* editable */ true);
    code->cfg().calculate_exit_block();
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(),
          constant_propagation::ConstantPrimitiveAndBoxedAnalyzer(
              &m_immut_analyzer_state, &m_immut_analyzer_state,
              constant_propagation::EnumFieldAnalyzerState::get(),
              constant_propagation::BoxedBooleanAnalyzerState::get(), nullptr));
      fp_iter.run({});
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      const_prop_stats += tf.apply(fp_iter, code->cfg(), method, &m_xstores);
    }
  }

  if (m_config.run_cse) {
    auto timer = m_cse_timer.scope();
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    cse_impl::CommonSubexpressionElimination cse(
        m_cse_shared_state.get(), code->cfg(), is_static(method),
        method::is_init(method) || method::is_clinit(method),
        method->get_class(), method->get_proto()->get_args());
    cse.patch();
    cse_stats = cse.get_stats();
  }

  if (m_config.run_copy_prop) {
    auto timer = m_copy_prop_timer.scope();
    copy_propagation_impl::Config config;
    copy_propagation_impl::CopyPropagation copy_propagation(config);
    copy_prop_stats = copy_propagation.run(code, method);
  }

  if (m_config.run_local_dce) {
    auto timer = m_local_dce_timer.scope();
    // LocalDce doesn't care if editable_cfg_built
    auto local_dce = LocalDce(m_pure_methods);
    local_dce.dce(code);
    local_dce_stats = local_dce.get_stats();
  }

  using stats_t = std::tuple<size_t, size_t, size_t, size_t>;
  auto get_features = [&code](size_t mminl_level) -> stats_t {
    if (!traceEnabled(MMINL, mminl_level)) {
      return stats_t{};
    }
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable= */ true);
    }
    const auto& cfg = code->cfg();

    size_t regs_before = cfg.get_registers_size();
    size_t insn_before = cfg.num_opcodes();
    size_t blocks_before = cfg.num_blocks();
    size_t edges = cfg.num_edges();
    return stats_t{regs_before, insn_before, blocks_before, edges};
  };

  constexpr size_t kMMINLDataCollectionLevel = 10;
  auto data_before_reg_alloc = get_features(kMMINLDataCollectionLevel);

  size_t reg_alloc_inc{0};
  if (m_config.run_reg_alloc) {
    if (should_shrink(code, m_forest) ||
        traceEnabled(MMINL, kMMINLDataCollectionLevel)) {
      auto timer = m_reg_alloc_timer.scope();

      // It's OK to ensure we have an editable CFG, the allocator would build
      // it, too.
      auto before_features = get_features(4);

      auto config = regalloc::graph_coloring::Allocator::Config{};
      config.no_overwrite_this = true; // Downstream passes may rely on this.
      regalloc::graph_coloring::allocate(config, method);
      // After this, any CFG is gone.

      // Assume that dedup will run, so building CFG is OK.
      auto after_features = get_features(4);
      TRACE(MMINL, 4,
            "Inliner.RegAlloc: %s: (%zu, %zu, %zu) -> (%zu, %zu, %zu)",
            SHOW(method), std::get<0>(before_features),
            std::get<1>(before_features), std::get<2>(before_features),
            std::get<0>(after_features), std::get<1>(after_features),
            std::get<2>(after_features));

      reg_alloc_inc = 1;
    }
  }

  if (m_config.run_fast_reg_alloc) {
    auto timer = m_fast_reg_alloc_timer.scope();
    auto allocator = fastregalloc::LinearScanAllocator(method);
    allocator.allocate();
  }

  if (m_config.run_dedup_blocks) {
    auto timer = m_dedup_blocks_timer.scope();
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    dedup_blocks_impl::Config config;
    dedup_blocks_impl::DedupBlocks dedup_blocks(&config, method);
    dedup_blocks.run();
    dedup_blocks_stats = dedup_blocks.get_stats();
  }

  auto data_after_dedup = get_features(kMMINLDataCollectionLevel);
  if (traceEnabled(MMINL, kMMINLDataCollectionLevel)) {
    TRACE(
        MMINL, kMMINLDataCollectionLevel,
        "Inliner.RegDedupe %zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu",
        std::get<0>(data_before_reg_alloc), std::get<1>(data_before_reg_alloc),
        std::get<2>(data_before_reg_alloc), std::get<3>(data_before_reg_alloc),
        std::get<0>(data_after_dedup), std::get<1>(data_after_dedup),
        std::get<2>(data_after_dedup), std::get<3>(data_after_dedup));
  }

  if (editable_cfg_built && !code->editable_cfg_built()) {
    code->build_cfg(/* editable */ true);
  } else if (!editable_cfg_built && code->editable_cfg_built()) {
    code->clear_cfg();
  }

  std::lock_guard<std::mutex> guard(m_stats_mutex);
  m_const_prop_stats += const_prop_stats;
  m_cse_stats += cse_stats;
  m_copy_prop_stats += copy_prop_stats;
  m_local_dce_stats += local_dce_stats;
  m_dedup_blocks_stats += dedup_blocks_stats;
  m_methods_shrunk++;
  m_methods_reg_alloced += reg_alloc_inc;
}

void Shrinker::log_metrics(ScopedMetrics& sm) const {
  auto scope = sm.scope("shrinker");
  m_const_prop_stats.log_metrics(sm);
}

} // namespace shrinker
