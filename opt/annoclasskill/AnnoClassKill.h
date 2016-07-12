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

class AnnoClassKillPass : public Pass {
 public:
  AnnoClassKillPass() : Pass("AnnoClassKillPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("kill_annos", {}, m_kill_annos);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&, PassManager&) override;

 private:
  std::vector<std::string> m_kill_annos;
};
