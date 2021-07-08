/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "ReduceBooleanBranches.h"

class ReduceBooleanBranchesPass : public Pass {
 public:
  ReduceBooleanBranchesPass() : Pass("ReduceBooleanBranchesPass") {}
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  reduce_boolean_branches_impl::Config m_config;
};
