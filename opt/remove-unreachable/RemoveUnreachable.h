/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Reachability.h"

class RemoveUnreachablePass : public Pass {
 public:
  RemoveUnreachablePass() : Pass("RemoveUnreachablePass") {}

  virtual void configure_pass(const JsonWrapper& jw) override {
    m_ignore_sets = reachability::IgnoreSets(jw);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  reachability::IgnoreSets m_ignore_sets;
};
