/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Shrinker.h"

#include "RegisterAllocation.h"

namespace shrinker {

Shrinker::Shrinker(
    DexStoresVector& stores,
    const Scope& scope,
    const ShrinkerConfig& config,
    const std::unordered_set<DexMethodRef*>& configured_pure_methods,
    const std::unordered_set<DexString*>& configured_finalish_field_names)
    : m_xstores(stores),
      m_config(config),
      m_enabled(config.run_const_prop || config.run_cse ||
                config.run_copy_prop || config.run_local_dce ||
                config.run_reg_alloc || config.run_dedup_blocks),
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
    if (editable_cfg_built) {
      code->clear_cfg();
    }
    if (!code->cfg_built()) {
      code->build_cfg(/* editable */ false);
    }
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
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
          code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      const_prop_stats += tf.apply(fp_iter, code->cfg(), method, &m_xstores);
    }
  }

  if (m_config.run_cse) {
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
    copy_propagation_impl::Config config;
    copy_propagation_impl::CopyPropagation copy_propagation(config);
    copy_prop_stats = copy_propagation.run(code, method);
  }

  if (m_config.run_local_dce) {
    // LocalDce doesn't care if editable_cfg_built
    auto local_dce = LocalDce(m_pure_methods);
    local_dce.dce(code);
    local_dce_stats = local_dce.get_stats();
  }

  if (m_config.run_reg_alloc) {
    auto get_features = [&code]() -> std::tuple<size_t, size_t, size_t> {
      if (!traceEnabled(MMINL, 4)) {
        return std::make_tuple(0u, 0u, 0u);
      }
      if (!code->editable_cfg_built()) {
        code->build_cfg(/* editable= */ true);
      }
      const auto& cfg = code->cfg();

      size_t regs_before = cfg.get_registers_size();
      size_t insn_before = cfg.num_opcodes();
      size_t blocks_before = cfg.num_blocks();
      return std::make_tuple(regs_before, insn_before, blocks_before);
    };

    // It's OK to ensure we have an editable CFG, the allocator would build it,
    // too.
    auto before_features = get_features();

    auto config = regalloc::graph_coloring::Allocator::Config{};
    config.no_overwrite_this = true; // Downstream passes may rely on this.
    regalloc::graph_coloring::allocate(config, method);
    // After this, any CFG is gone.

    // Assume that dedup will run, so building CFG is OK.
    auto after_features = get_features();
    TRACE(MMINL, 4, "Inliner.RegAlloc: %s: (%zu, %zu, %zu) -> (%zu, %zu, %zu)",
          SHOW(method), std::get<0>(before_features),
          std::get<1>(before_features), std::get<2>(before_features),
          std::get<0>(after_features), std::get<1>(after_features),
          std::get<2>(after_features));
  }

  if (m_config.run_dedup_blocks) {
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    dedup_blocks_impl::Config config;
    dedup_blocks_impl::DedupBlocks dedup_blocks(&config, method);
    dedup_blocks.run();
    dedup_blocks_stats = dedup_blocks.get_stats();
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
}

} // namespace shrinker
