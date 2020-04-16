/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MakePublicPass.h"

#include "DexAccess.h"
#include "DexUtil.h"

void MakePublicPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& conf,
                              PassManager& mgr) {
  auto scope = build_class_scope(stores);
  loosen_access_modifier(scope);
}

static MakePublicPass s_pass;
