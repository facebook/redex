/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "LocalDce.h"
#include "Pass.h"

class LocalDcePass : public Pass {
 public:
  LocalDcePass() : Pass("LocalDcePass") {}

  bool no_implementor_abstract_is_pure{false};

  void bind_config() override {
    bind("no_implementor_abstract_is_pure",
         false,
         no_implementor_abstract_is_pure);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  std::unordered_set<DexMethodRef*> find_pure_methods(const Scope&);
};
