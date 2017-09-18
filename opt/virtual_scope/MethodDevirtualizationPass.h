/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

class MethodDevirtualizationPass : public Pass {
 public:
  MethodDevirtualizationPass() : Pass("MethodDevirtualizationPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("staticize_vmethods_not_using_this",
           true,
           m_staticize_vmethods_not_using_this);
    pc.get("staticize_vmethods_using_this",
           false,
           m_staticize_vmethods_using_this);
    pc.get("staticize_dmethods_not_using_this",
           true,
           m_staticize_dmethods_not_using_this);
    pc.get("staticize_dmethods_using_this",
           false,
           m_staticize_dmethods_using_this);
    pc.get("ignore_keep", false, m_ignore_keep);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_staticize_vmethods_not_using_this;
  bool m_staticize_vmethods_using_this;
  bool m_staticize_dmethods_not_using_this;
  bool m_staticize_dmethods_using_this;
  bool m_ignore_keep;
};
