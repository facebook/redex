/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "RedexProperties.h"

/**
 * This pass detects classes whose <clinit> (class initializer) method
 * unconditionally throws an exception. Such classes are problematic because
 * they will fail to load at runtime if ever initialized.
 *
 * This is useful for identifying dead code that is covered by keep rules
 * but would fail if actually used, allowing application authors to prioritize
 * manual cleanups.
 */
class UnconditionallyThrowingClassesPass : public Pass {
 public:
  UnconditionallyThrowingClassesPass()
      : Pass("UnconditionallyThrowingClassesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  std::string get_config_doc() override {
    return "This pass detects classes whose <clinit> (class initializer) "
           "method "
           "unconditionally throws an exception. Such classes are problematic "
           "because they will fail to load at runtime if ever initialized. "
           "This is useful for identifying dead code that is covered by keep "
           "rules but would fail if actually used.";
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
