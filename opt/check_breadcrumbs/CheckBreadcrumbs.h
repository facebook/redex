/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * This pass only makes sense when applied at the end of a redex optimization
 * run. It does not work on its own when applied to a "random" apk.
 * It relies on the fact that deleted classes/methods/fields are still
 * around at the end of a run.
 */
class CheckBreadcrumbsPass : public Pass {
 public:
  CheckBreadcrumbsPass() : Pass("CheckBreadcrumbsPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("fail", false, fail);
    pc.get("fail_if_illegal_refs", false, fail_if_illegal_refs);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

private:
  bool fail;
  bool fail_if_illegal_refs;
};
