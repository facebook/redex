/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "AccessMarking.h"

#include "DexUtil.h"
#include "ReachableClasses.h"

namespace {
size_t mark_classes_final(const DexStoresVector& stores) {
  size_t n_classes_finalized = 0;
  for (auto const& dex : DexStoreClassesIterator(stores)) {
    for (auto const& cls : dex) {
      if (is_seed(cls) || is_abstract(cls) || is_final(cls)) continue;
      auto const& children = get_children(cls->get_type());
      if (children.empty()) {
        set_final(cls);
        ++n_classes_finalized;
      }
    }
  }
  return n_classes_finalized;
}
}

void AccessMarkingPass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& cfg,
  PassManager& pm
) {
  if (!cfg.using_seeds) {
    return;
  }
  pm.incr_metric("finalized_classes", mark_classes_final(stores));
}

static AccessMarkingPass s_pass;
