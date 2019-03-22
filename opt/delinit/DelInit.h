/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class DelInitPass : public Pass {
 public:
  DelInitPass() : Pass("DelInitPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("package_white_list", {}, m_package_filter);
  }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_package_filter;
};
