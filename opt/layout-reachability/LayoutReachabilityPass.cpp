/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LayoutReachabilityPass.h"

#include "ConfigFiles.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Trace.h"

void LayoutReachabilityPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager& mgr) {
  TRACE(PGR, 1, "Recomputing layout classes");
  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);
  always_assert(!apk_dir.empty());

  auto scope = build_class_scope(stores);
  // Update the m_byresources rstate flags on classes/methods
  recompute_reachable_from_xml_layouts(scope, apk_dir);
}

static LayoutReachabilityPass s_pass;
