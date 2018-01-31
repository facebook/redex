/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Walkers.h"

using namespace constant_propagation;

void ConstantPropagationPass::configure_pass(const PassConfig& pc) {
  pc.get(
      "replace_moves_with_consts", false, m_config.replace_moves_with_consts);
  pc.get("fold_arithmetic", false, m_config.fold_arithmetic);
  pc.get("propagate_conditions", false, m_config.propagate_conditions);
  std::vector<std::string> blacklist_names;
  pc.get("blacklist", {}, blacklist_names);

  for (auto const& name : blacklist_names) {
    DexType* entry = DexType::get_type(name.c_str());
    if (entry) {
      TRACE(CONSTP, 2, "Blacklisted class: %s\n", SHOW(entry));
      m_config.blacklist.insert(entry);
    }
  }
}

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);

  using Data = std::nullptr_t;
  auto stats = walk::parallel::reduce_methods<Data, Transform::Stats>(
      scope,
      [&](Data&, DexMethod* method) {
        if (method->get_code() == nullptr) {
          return Transform::Stats();
        }
        auto& code = *method->get_code();
        // Skipping blacklisted classes
        if (m_config.blacklist.count(method->get_class()) > 0) {
          TRACE(CONSTP, 2, "Skipping %s\n", SHOW(method));
          return Transform::Stats();
        }

        TRACE(CONSTP, 2, "Method: %s\n", SHOW(method));

        code.build_cfg();
        auto& cfg = code.cfg();

        TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
        intraprocedural::FixpointIterator fp_iter(cfg, m_config);
        fp_iter.run(ConstantEnvironment());
        constant_propagation::Transform tf(m_config);
        return tf.apply(fp_iter, &code);
      },

      [](Transform::Stats a, Transform::Stats b) { // reducer
        return a + b;
      },
      [&](unsigned int) { // data initializer
        return nullptr;
      });

  mgr.incr_metric("num_branch_propagated", stats.branches_removed);
  mgr.incr_metric("num_materialized_consts", stats.materialized_consts);

  TRACE(CONSTP, 1, "num_branch_propagated: %d\n", stats.branches_removed);
  TRACE(CONSTP,
        1,
        "num_moves_replaced_by_const_loads: %d\n",
        stats.materialized_consts);
}

static ConstantPropagationPass s_pass;
