/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantPropagationWholeProgramState.h"
#include "IRCode.h"
#include "Pass.h"

class FinalInlinePassV2 : public Pass {
 public:
  FinalInlinePassV2() : Pass("FinalInlinePassV2") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("aggressively_delete", true, m_aggressively_delete);
  }

  static size_t run(const Scope&);

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_aggressively_delete{true};
};

namespace final_inline {

class class_initialization_cycle : public std::exception {
 public:
  class_initialization_cycle(const DexClass* cls) {
    m_msg = "Found a class initialization cycle involving " + show(cls);
  }

  const char* what() const noexcept override { return m_msg.c_str(); }

 private:
  std::string m_msg;
};

constant_propagation::WholeProgramState analyze_and_simplify_clinits(
    const Scope& scope);

} // namespace final_inline
