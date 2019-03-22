/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class RemoveGotosPass : public Pass {
 public:
  RemoveGotosPass() : Pass("RemoveGotosPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  size_t run(DexMethod*);
};
