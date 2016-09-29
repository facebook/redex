/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class LocalDcePass : public Pass {
 public:
  LocalDcePass()
    : Pass("LocalDcePass", DoesNotSync{}) {}

  static void run(DexMethod* method);

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
