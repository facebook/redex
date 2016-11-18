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

class RemoveUnreachablePass : public Pass {
 public:
  RemoveUnreachablePass() : Pass("RemoveUnreachablePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("classes_removed", true, m_classes_removed);
    pc.get("fields_removed", true, m_fields_removed);
    pc.get("methods_removed", true, m_methods_removed);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  private:
    bool m_classes_removed;
    bool m_fields_removed;
    bool m_methods_removed;
};
