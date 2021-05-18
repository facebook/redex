/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Model.h"

#include "AnonymousClassMergingPass.h"

#include "AnonymousModelGenerator.h"
#include "ClassMerging.h"
#include "ConfigFiles.h"
#include "MergingStrategies.h"
#include "PassManager.h"

namespace class_merging {

void AnonymousClassMergingPass::bind_config() {
  bind("exclude",
       {},
       m_merging_spec.exclude_types,
       "Do not merge the classes or its implementors");
  bind("min_eligible",
       500,
       m_min_eligible_classes,
       "Strip out interfaces or supertypes with less than min_eligible "
       "implementors or subclasses");
  bind("include_primary_dex", false, m_merging_spec.include_primary_dex);
  bind("allowed_packages",
       {},
       allowed_packages,
       "Packages of types that are allowed to be merged, default is all "
       "pakcages");
}

void AnonymousClassMergingPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles& conf,
                                         PassManager& mgr) {
  // Fill the merging configurations.
  m_merging_spec.name = "Anonymous Classes";
  m_merging_spec.class_name_prefix = "Anon";
  m_merging_spec.merge_per_interdex_set = InterDexGroupingType::NON_ORDERED_SET;
  if (conf.force_single_dex()) {
    m_merging_spec.include_primary_dex = true;
  }
  m_merging_spec.dedup_throw_blocks = false;

  discover_mergeable_anonymous_classes(
      stores, allowed_packages, m_min_eligible_classes, &m_merging_spec, &mgr);
  if (!m_merging_spec.roots.empty()) {
    strategy::set_merging_strategy(strategy::BY_REFS);

    auto scope = build_class_scope(stores);
    class_merging::merge_model(scope, conf, mgr, stores, m_merging_spec);
    post_dexen_changes(scope, stores);
  } else {
    TRACE(CLMG, 2, "No enough anonymous classes to merge");
  }
}

static AnonymousClassMergingPass s_pass;

} // namespace class_merging
