/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LayoutReachabilityPass.h"
#include "ReachableClasses.h"

void LayoutReachabilityPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  TRACE(PGR, 1, "Recomputing layout classes\n");
  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);
  always_assert(apk_dir.size());

  auto scope = build_class_scope(stores);
  // Update the m_byresources rstate flags on classes/methods
  recompute_reachable_from_xml_layouts(scope, apk_dir);
}

static LayoutReachabilityPass s_pass;
