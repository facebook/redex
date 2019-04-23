/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class BridgePass : public Pass {
 public:
  BridgePass() : Pass("BridgePass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("black_list", {}, m_black_list);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  // Prefixes of classes not to bridge to
  std::vector<std::string> m_black_list;
};
