/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#ifndef StaticReloPass_h
#define StaticReloPass_h

#include "PassManager.h"
#include "DexClass.h"

class StaticReloPass : public Pass {
 public:
  StaticReloPass() : Pass("StaticReloPass") {}

  virtual void run_pass(DexClassesVector&, PgoFiles&) override;
};

#endif
