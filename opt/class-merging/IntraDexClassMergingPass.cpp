/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include "ClassMerging.h"
#include "ConfigUtils.h"
#include "InterDexGrouping.h"
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
       4,
       m_global_min_count,
       "Ignore interface or class hierarchies with less than global_mint_count "
       "implementors or subclasses");
  bind("min_count",
       2,
       m_merging_spec.min_count,
       "Minimal number of mergeables to be merged together");
  size_t max_count;
  bind("max_count",
       50,
       max_count,
       "Maximum mergeable class count per merging group");
  if (max_count > 0) {
    m_merging_spec.max_count = boost::optional<size_t>(max_count);
  }
  bind("use_stable_shape_names", false, m_merging_spec.use_stable_shape_names);
  std::string interdex_grouping;
  bind("interdex_grouping", "non-ordered-set", interdex_grouping);
  // Inferring_mode is "class-loads" by default.
  m_merging_spec.interdex_config.init_type(interdex_grouping);
}

void IntraDexClassMergingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  // Fill the merging configurations.
  m_merging_spec.name = "Intra Dex";
  m_merging_spec.class_name_prefix = "IDx";
  // The merging strategy can be tuned.
  m_merging_spec.strategy = strategy::BY_CODE_SIZE;
  // TODO: Can merge FULL interdex groups.
  m_merging_spec.per_dex_grouping = true;
  m_merging_spec.dedup_fill_in_stack_trace = false;

  auto scope = build_class_scope(stores);
  TypeSystem type_system(scope);
  find_all_mergeables_and_roots(type_system, scope, m_global_min_count, mgr,
                                &m_merging_spec);
  if (m_merging_spec.roots.empty()) {
    TRACE(CLMG, 1, "No mergeable classes found by IntraDexClassMergingPass");
    return;
  }

  class_merging::merge_model(type_system, scope, conf, mgr, stores,
                             m_merging_spec);

  post_dexen_changes(scope, stores);

  // For interface roots, the num_roots count is not accurate. It counts the
  // total number of unique common base classes among the implementors, not the
  // common interface roots.
  mgr.set_metric("num_roots", m_merging_spec.roots.size());

  m_merging_spec.merging_targets.clear();
  m_merging_spec.roots.clear();
}

static IntraDexClassMergingPass s_intra_dex_merging_pass;
} // namespace class_merging
