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
  bind("min_implementors",
       500,
       m_min_implementor_size,
       "Strip out interfaces with less than min_implementors implementors");
}

void AnonymousClassMergingPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles& conf,
                                         PassManager& mgr) {
  if (!mgr.get_redex_options().is_art_build) {
    TRACE(CLMG, 1, "AnonymousClassMergingPass is enabled only for art builds");
    return;
  }
  auto scope = build_class_scope(stores);
  discover_mergeable_anonymous_classes(
      scope, m_min_implementor_size, &m_merging_spec, &mgr);
  if (!m_merging_spec.roots.empty()) {
    // Finishing the merging configurations.
    m_merging_spec.name = "Anonymous Classes";
    m_merging_spec.class_name_prefix = "Anon";
    m_merging_spec.merge_per_interdex_set =
        InterDexGroupingType::NON_ORDERED_SET;
    m_merging_spec.include_primary_dex =
        conf.get_json_config().get("force_single_dex", false);
    m_merging_spec.dedup_throw_blocks = false;
    strategy::set_merging_strategy(strategy::BY_CODE_SIZE);

    class_merging::merge_model(scope, conf, mgr, stores, m_merging_spec);
    post_dexen_changes(scope, stores);
  } else {
    TRACE(CLMG, 2, "No enough anonymous classes to merge");
  }
}

static AnonymousClassMergingPass s_pass;

} // namespace class_merging
