/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class VerifierPass : public Pass {
 public:
  VerifierPass() : Pass("VerifierPass") {}

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("class_dependencies_output", "", m_class_dependencies_output);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_class_dependencies_output;
};
