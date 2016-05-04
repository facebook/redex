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

class AnnoKillPass : public Pass {
 public:
  AnnoKillPass() : Pass("AnnoKillPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("remove_all_build_annos", false, m_remove_build);
    pc.get("remove_all_system_annos", false, m_remove_system);
    pc.get("remove_annos", {}, m_remove_annos);
    pc.get("blacklist", {}, m_blacklist);
  }

  virtual void run_pass(DexClassesVector&, ConfigFiles&) override;

 private:
  bool m_remove_build;
  bool m_remove_system;
  std::vector<std::string> m_remove_annos;
  std::vector<std::string> m_blacklist;
};
