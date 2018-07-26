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

  virtual void configure_pass(const JsonWrapper& jw) override {
    jw.get("output_file_name", "", m_output_file_name);
    jw.get("dump_detailed_info", false, m_dump_detailed_info);
    jw.get("ignore_string_literals", {}, m_ignore_string_literals);
    jw.get("ignore_string_literal_annos", {}, m_ignore_string_literal_annos);
    jw.get("ignore_system_annos", {}, m_ignore_system_annos);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_output_file_name;
  bool m_dump_detailed_info{true};
  std::vector<std::string> m_ignore_string_literals;
  std::vector<std::string> m_ignore_string_literal_annos;
  std::vector<std::string> m_ignore_system_annos;
};
