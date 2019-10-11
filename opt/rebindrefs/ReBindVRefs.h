/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ReBindVRefsPass : public Pass {
 public:
  ReBindVRefsPass() : Pass("ReBindVRefsPass") {}

  void bind_config() override {
    bind("desuperify", true, m_desuperify,
         "Convert invoke-super calls to invoke-virtual where possible");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_desuperify;
};
