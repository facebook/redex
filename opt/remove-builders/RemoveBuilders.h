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

class RemoveBuildersPass : public Pass {
 public:
  RemoveBuildersPass() : Pass("RemoveBuildersPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::unordered_set<DexType*> m_builders;

  std::vector<DexType*> created_builders(DexMethod*);
  bool escapes_stack(DexType*, DexMethod*);
};
