/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ReplaceGotosWithReturnsPass : public Pass {
 public:
  ReplaceGotosWithReturnsPass() : Pass("ReplaceGotosWithReturnsPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static size_t process_code(IRCode*);
};
