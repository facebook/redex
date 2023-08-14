/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

/**
 * This pass is meant to edit layout .xml files to replace things of the form
 * <View ...> with <view class="android.view.View" ...> to avoid class load
 * attempts for obviously non-existent classes.
 */
class FullyQualifyLayouts : Pass {
 public:
  FullyQualifyLayouts() : Pass("FullyQualifyLayoutsPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
