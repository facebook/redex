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
constexpr int REGSIZE = 256;

// The struct AbstractRegister contains a bool value of whether the value of register
// is known, a pointer to an insn and the constant value stored by this insn
struct AbstractRegister {
  bool known;
  DexInstruction* insn;
  int64_t val;
};

class ConstantPropagationPass : public Pass {
 public:
  ConstantPropagationPass()
    : Pass("ConstantPropagationPass", DoesNotSync{}) {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("blacklist", {}, m_blacklist);
  }
  virtual void run_pass(DexClassesVector&, ConfigFiles&) override;

private:
  std::vector<std::string> m_blacklist;
};
