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

class ReorderInterfacesPass : public Pass {
 public:
  ReorderInterfacesPass() : Pass("ReorderInterfacesPass") {}

  virtual void configure_pass(const PassConfig& /* unused */) override {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
