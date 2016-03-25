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

class SingleImplPass : public Pass {
 public:
  SingleImplPass() : Pass("SingleImplPass") {}

  virtual void run_pass(DexClassesVector&, PgoFiles&) override;

  // count of removed interfaces
  size_t removed_count{0};
  // count of invoke-interface changed to invoke-virtual
  static size_t s_invoke_intf_count;
};
