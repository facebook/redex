/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class Outliner : public Pass {
 public:
  Outliner() : Pass("Outliner") {}

  void configure_pass(const JsonWrapper& jw) override {
    // N.B. we pretty much never want to outline the primary dex, but
    // we need to allow this to happen in some scenarios, e.g.
    // instrumentation tests, since they are single-dex affairs.
    jw.get("outline_primary_dex", false, m_outline_primary_dex);
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  bool m_outline_primary_dex;
};
