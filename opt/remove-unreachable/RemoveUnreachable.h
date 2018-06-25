/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class RemoveUnreachablePass : public Pass {
 public:
  RemoveUnreachablePass() : Pass("RemoveUnreachablePass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("ignore_string_literals", {}, m_ignore_string_literals);
    pc.get("ignore_string_literal_annos", {}, m_ignore_string_literal_annos);
    pc.get("ignore_system_annos", {}, m_ignore_system_annos);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  private:
    std::vector<std::string> m_ignore_string_literals;
    std::vector<std::string> m_ignore_string_literal_annos;
    std::vector<std::string> m_ignore_system_annos;
};
