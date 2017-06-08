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

class RedundantMoveEliminationPass : public Pass {
 public:
  RedundantMoveEliminationPass() : Pass("RedundantMoveEliminationPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("eliminate_const_literals", true, m_config.eliminate_const_literals);
    pc.get("eliminate_const_strings", true, m_config.eliminate_const_strings);
    pc.get("eliminate_const_classes", true, m_config.eliminate_const_classes);
    pc.get("replace_with_representative",
           true,
           m_config.replace_with_representative);
    pc.get("full_method_analysis", true, m_config.full_method_analysis);
  }

  struct Config {
    bool eliminate_const_literals;
    bool eliminate_const_strings;
    bool eliminate_const_classes;
    bool replace_with_representative;
    bool full_method_analysis;
  } m_config;
};
