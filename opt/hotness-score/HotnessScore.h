/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
