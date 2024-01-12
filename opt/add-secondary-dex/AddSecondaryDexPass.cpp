/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AddSecondaryDexPass.h"
#include "InterDexPass.h"

void AddSecondaryDexPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles&,
                                   PassManager&) {
  redex_assert(!stores.empty());
  auto& root_dexen = stores[0].get_dexen();
  root_dexen.emplace_back(1, create_canary(root_dexen.size()));
}

static AddSecondaryDexPass s_pass;
