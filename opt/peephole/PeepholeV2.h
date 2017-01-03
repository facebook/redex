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

class PeepholePassV2 : public Pass {
 public:
  PeepholePassV2()
    : Pass("PeepholePassV2", DoesNotSync{}) {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
