/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexPropertyChecker.h"

namespace redex_properties {

class DexLimitsChecker : public PropertyChecker {
 public:
  DexLimitsChecker() : PropertyChecker(names::DexLimitsObeyed) {}

  void run_checker(DexStoresVector&, ConfigFiles&, PassManager&, bool) override;
};

} // namespace redex_properties
