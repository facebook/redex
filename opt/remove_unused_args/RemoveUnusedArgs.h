/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "PassManager.h"

namespace remove_unused_args {

class RemoveUnusedArgsPass : public Pass {
 public:
  RemoveUnusedArgsPass() : Pass("RemoveUnusedArgsPass") {}

  virtual void run_pass(DexStoresVector&,
                        ConfigFiles&,
                        PassManager& mgr) override;
};

} // namespace remove_unused_args
