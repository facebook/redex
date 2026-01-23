/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinTrivialLambdaDeduplicationPass.h"

#include "ConfigFiles.h"
#include "PassManager.h"

void KotlinTrivialLambdaDeduplicationPass::run_pass(
    DexStoresVector& /* stores */,
    ConfigFiles& /* conf */,
    PassManager& /* mgr */) {
  // TODO: Implementation will be added in the next diff.
}

static KotlinTrivialLambdaDeduplicationPass s_pass;
