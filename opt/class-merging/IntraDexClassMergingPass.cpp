/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include "ClassMerging.h"
#include "ConfigUtils.h"
#include "IntraDexClassMergingPass.h"
#include "ModelSpecGenerator.h"
#include "PassManager.h"

namespace class_merging {
void IntraDexClassMergingPass::bind_config() {
  std::vector<std::string> excl_names;
  bind("exclude",
       {},
       excl_names,
       "Do not merge the classes or its implementors");
  utils::load_types_and_prefixes(excl_names, m_merging_spec.exclude_types,
                                 m_merging_spec.exclude_prefixes);
  bind("global_min_count",
       50,
       m_global_min_count,
       "Ignore interface or class hierarchies with less than global_mint_count "
       "implementors or subclasses");
  bind("min_count",
       10,
       m_merging_spec.min_count,
       "Minimal number of mergeables to be merged together");
  std::string interdex_grouping;
  bind("interdex_grouping", "non-ordered-set", interdex_grouping);
  m_merging_spec.interdex_grouping =
      get_merge_per_interdex_type(interdex_grouping);
}

void IntraDexClassMergingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(CLMG,
          1,
          "IntraDexClassMergingPass not run because no "
          "ProGuard configuration was provided.");
    return;
  }
  // Fill the merging configurations.
  m_merging_spec.name = "Intra Dex";
  m_merging_spec.class_name_prefix = "IDx";
  // The merging strategy can be tuned.
  m_merging_spec.strategy = strategy::BY_CODE_SIZE;
  // Can merge FULL interdex groups.
  m_merging_spec.dedup_fill_in_stack_trace = false;
  m_merging_spec.per_dex_grouping = true;
  auto scope = build_class_scope(stores);
  TypeSystem type_system(scope);
  find_all_mergeables_and_roots(type_system, scope, m_global_min_count,
                                &m_merging_spec);
  if (m_merging_spec.roots.empty()) {
    TRACE(CLMG, 1, "No mergeable classes found by IntraDexClassMergingPass");
    return;
  }

  class_merging::merge_model(type_system, scope, conf, mgr, stores,
                             m_merging_spec);

  post_dexen_changes(scope, stores);

  mgr.set_metric("num_roots", m_merging_spec.roots.size());

  m_merging_spec.merging_targets.clear();
  m_merging_spec.roots.clear();
}

static IntraDexClassMergingPass s_intra_dex_merging_pass;
} // namespace class_merging
