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

namespace class_merging {

void AnonymousClassMergingPass::bind_config() {
  bind("exclude", {}, m_merging_spec.exclude_types);
}

void AnonymousClassMergingPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles& conf,
                                         PassManager& mgr) {
  auto scope = build_class_scope(stores);
  discover_mergeable_anonymous_classes(scope, &m_merging_spec);
  if (!m_merging_spec.roots.empty()) {
    class_merging::merge_model(scope, conf, mgr, stores, m_merging_spec);
    post_dexen_changes(scope, stores);
  } else {
    TRACE(CLMG, 2, "No enough anonymous classes to merge");
  }
}

static AnonymousClassMergingPass s_pass;

} // namespace class_merging
