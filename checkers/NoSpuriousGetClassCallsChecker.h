/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexPropertyChecker.h"

namespace redex_properties {

class NoSpuriousGetClassCallsChecker : public PropertyChecker {
 public:
  NoSpuriousGetClassCallsChecker()
      : PropertyChecker(names::NoSpuriousGetClassCalls) {}

  void run_checker(DexStoresVector&, ConfigFiles&, PassManager&, bool) override;

 private:
  DexMethodRef* m_getClass_ref;
  void check_spurious_getClass(DexMethod* method);
};

} // namespace redex_properties
