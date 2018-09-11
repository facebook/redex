/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Reachability.h"

class ReachabilityGraphPrinterPass : public Pass {
 public:
  ReachabilityGraphPrinterPass() : Pass("ReachabilityGraphPrinterPass") {}

  virtual void configure_pass(const JsonWrapper& jw) override {
    jw.get("output_file_name", "", m_output_file_name);
    jw.get("dump_detailed_info", false, m_dump_detailed_info);
    m_ignore_sets = reachability::IgnoreSets(jw);
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_output_file_name;
  bool m_dump_detailed_info{true};
  reachability::IgnoreSets m_ignore_sets;
};
