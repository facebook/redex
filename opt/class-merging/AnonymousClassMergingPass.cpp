/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include "AnonymousClassMergingPass.h"

#include "ClassMerging.h"
#include "ConfigFiles.h"
#include "ConfigUtils.h"
#include "MergingStrategies.h"
#include "ModelSpecGenerator.h"
#include "PassManager.h"

namespace class_merging {

void AnonymousClassMergingPass::bind_config() {
  std::vector<std::string> excl_names;
  bind("exclude",
       {},
       excl_names,
       "Do not merge the classes or its implementors");
  utils::load_types_and_prefixes(excl_names, m_merging_spec.exclude_types,
                                 m_merging_spec.exclude_prefixes);
  bind("global_min_count",
       500,
       m_global_min_count,
       "Ignore interface or class hierarchies with less than "
       "global_min_count implementors or subclasses");
  bind("include_primary_dex", false, m_merging_spec.include_primary_dex);
  bind("allowed_packages",
       {},
       allowed_packages,
       "Packages of types that are allowed to be merged, default is all "
       "pakcages");
  bind("min_count",
       2,
       m_min_count,
       "Minimum mergeable class count per merging group");
  std::string merge_per_interdex_set;
  bind("merge_per_interdex_set", "non-ordered-set", merge_per_interdex_set);
  m_merging_spec.merge_per_interdex_set =
      get_merge_per_interdex_type(merge_per_interdex_set);
}

void AnonymousClassMergingPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles& conf,
                                         PassManager& mgr) {
  // Fill the merging configurations.
  m_merging_spec.name = "Anonymous Classes";
  m_merging_spec.class_name_prefix = "Anon";
  m_merging_spec.strategy = strategy::BY_REFS;
  if (conf.force_single_dex() ||
      (!stores.empty() && stores[0].num_dexes() == 1)) {
    m_merging_spec.include_primary_dex = true;
  }
  m_merging_spec.dedup_throw_blocks = false;
  m_merging_spec.min_count = m_min_count;

  discover_mergeable_anonymous_classes(
      stores, allowed_packages, m_global_min_count, &m_merging_spec, &mgr);
  if (!m_merging_spec.roots.empty()) {
    auto scope = build_class_scope(stores);
    class_merging::merge_model(scope, conf, mgr, stores, m_merging_spec);
    post_dexen_changes(scope, stores);
  } else {
    TRACE(CLMG, 2, "No enough anonymous classes to merge");
  }
}

static AnonymousClassMergingPass s_pass;

} // namespace class_merging
