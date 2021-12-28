/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class LayoutReachabilityPass : Pass {
 public:
  LayoutReachabilityPass() : Pass("LayoutReachabilityPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
