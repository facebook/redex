/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinStatelessLambdaSingletonRemovalPass.h"

void KotlinStatelessLambdaSingletonRemovalPass::run_pass(
    DexStoresVector& stores, ConfigFiles& conf, PassManager& mgr) {}

static KotlinStatelessLambdaSingletonRemovalPass s_pass;
