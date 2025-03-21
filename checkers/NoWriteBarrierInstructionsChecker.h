/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexPropertyChecker.h"

namespace redex_properties {

class NoWriteBarrierInstructionsChecker : public PropertyChecker {
 public:
  NoWriteBarrierInstructionsChecker()
      : PropertyChecker(names::NoWriteBarrierInstructions) {}

  void run_checker(DexStoresVector&, ConfigFiles&, PassManager&, bool) override;
};

} // namespace redex_properties
