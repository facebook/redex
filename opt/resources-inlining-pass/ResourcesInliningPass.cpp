/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourcesInliningPass.h"
#include "Trace.h"

void ResourcesInliningPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  for (const auto& elem : m_resource_type_names) {
    TRACE(RIP, 1, "Resource Type Name: %s", elem.c_str());
  }

  for (const auto& elem : m_resource_entry_names) {
    TRACE(RIP, 1, "Resource Entry Name: %s", elem.c_str());
  }
}

static ResourcesInliningPass s_pass;
