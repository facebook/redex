/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class ReachabilityGraphPrinterPass : public Pass {
 public:
  ReachabilityGraphPrinterPass() : Pass("ReachabilityGraphPrinterPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("output_file_name", "", m_output_file_name);
    pc.get("dump_detailed_info", false, m_dump_detailed_info);
    pc.get("ignore_string_literals", {}, m_ignore_string_literals);
    pc.get("ignore_string_literal_annos", {}, m_ignore_string_literal_annos);
    pc.get("ignore_system_annos", {}, m_ignore_system_annos);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_output_file_name;
  bool m_dump_detailed_info{true};
  std::vector<std::string> m_ignore_string_literals;
  std::vector<std::string> m_ignore_string_literal_annos;
  std::vector<std::string> m_ignore_system_annos;
};
