/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourceValueMergingPass.h"
#include "DeterministicContainers.h"
#include "Pass.h"
#include "PassManager.h"
#include "Trace.h"

void ResourceValueMergingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  TRACE(RES, 1, "ResourceValueMergingPass excluded_resources count: %zu",
        m_excluded_resources.size());

  for (const auto& resource : UnorderedIterable(m_excluded_resources)) {
    TRACE(RES, 1, "  Excluded resource: %s", resource.c_str());
  }
}

static ResourceValueMergingPass s_pass;
