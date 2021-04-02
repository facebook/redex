/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReduceBooleanBranchesPass.h"

#include "CopyPropagation.h"
#include "DexClass.h"
#include "LocalDce.h"
#include "PassManager.h"
#include "Purity.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_BOOLEAN_BRANCHES_REMOVED =
    "num_boolean_branches_removed";
constexpr const char* METRIC_OBJECT_BRANCHES_REMOVED =
    "num_object_branches_removed";
constexpr const char* METRIC_XORS_REDUCED = "num_xors_reduced";

} // namespace

void ReduceBooleanBranchesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles& /* unused */,
                                         PassManager& mgr) {
  auto scope = build_class_scope(stores);

  copy_propagation_impl::Config copy_prop_config;
  copy_prop_config.eliminate_const_classes = false;
  copy_prop_config.eliminate_const_strings = false;
  copy_prop_config.static_finals = false;
  auto pure_methods = get_pure_methods();
  auto stats = walk::parallel::methods<reduce_boolean_branches_impl::Stats>(
      scope, [&config = m_config, &copy_prop_config,
              &pure_methods](DexMethod* method) {
        const auto code = method->get_code();
        if (!code) {
          return reduce_boolean_branches_impl::Stats{};
        }

        code->build_cfg(/* editable */ true);
        std::shared_ptr<ab_test::ABExperimentContext> experiment;
        std::function<void()> on_change = [&experiment, code, method]() {
          if (!experiment) {
            experiment = ab_test::ABExperimentContext::create(
                &code->cfg(), method, "reduce_boolean_branches");
          }
        };

        reduce_boolean_branches_impl::ReduceBooleanBranches rbb(
            config, is_static(method), method->get_proto()->get_args(), code,
            &on_change);
        while (rbb.run()) {
          // clean up
          copy_propagation_impl::CopyPropagation copy_propagation(
              copy_prop_config);
          copy_propagation.run(code, method);
          LocalDce(pure_methods).dce(code);
        }

        if (experiment) {
          experiment->flush();
        } else {
          code->clear_cfg();
        }
        return rbb.get_stats();
      });

  mgr.incr_metric(METRIC_BOOLEAN_BRANCHES_REMOVED,
                  stats.boolean_branches_removed);
  mgr.incr_metric(METRIC_OBJECT_BRANCHES_REMOVED,
                  stats.object_branches_removed);
  mgr.incr_metric(METRIC_XORS_REDUCED, stats.xors_reduced);
  TRACE(RBB, 1,
        "[reduce boolean branches] Removed %zu boolean branches, %zu object "
        "branches, reduced %zu xors",
        stats.boolean_branches_removed, stats.object_branches_removed,
        stats.xors_reduced);
}

static ReduceBooleanBranchesPass s_pass;
