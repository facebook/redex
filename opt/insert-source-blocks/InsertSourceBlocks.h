/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

// A pass to insert SourceBlock MIEs into CFGs.
//
// This is a pass so it can be more freely scheduled. A simple example is
// to run this *after* the first RemoveUnreachables pass, so as to not
// create unnecessary bloat.
class InsertSourceBlocksPass : public Pass {
 public:
  InsertSourceBlocksPass() : Pass("InsertSourceBlocksPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
