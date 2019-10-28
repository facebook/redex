/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * This pass makes makes everything that's package-private public,
 * RenameClassesPassV2 depends on this transformation to work correctly. We
 * extract this logic out to do the same without renaming.
 */
class MakePublicPass : public Pass {
 public:
  MakePublicPass() : Pass("MakePublicPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
