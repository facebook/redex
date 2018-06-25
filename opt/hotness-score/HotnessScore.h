/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include <string>

class HotnessScorePass : public Pass {
 public:
  HotnessScorePass() : Pass("HotnessScorePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("warm_marker", "", m_warm_marker);
    pc.get("mild_marker", "", m_mild_marker);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_warm_marker;
  std::string m_mild_marker;
};
