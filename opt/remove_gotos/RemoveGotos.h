/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class RemoveGotosPass : public Pass {
 public:
  RemoveGotosPass() : Pass("RemoveGotosPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  size_t run(DexMethod*);
};
