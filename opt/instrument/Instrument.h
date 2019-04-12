/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "PassManager.h"

#include <unordered_set>

class InstrumentPass : public Pass {
 public:
  InstrumentPass() : Pass("InstrumentPass") {}

  void configure_pass(const JsonWrapper& jw) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Options {
    std::string instrumentation_strategy;
    std::string analysis_class_name;
    std::string analysis_method_name;
    std::unordered_set<std::string> blacklist;
    std::unordered_set<std::string> whitelist;
    std::string blacklist_file_name;
    std::string metadata_file_name;
    int64_t num_stats_per_method;
    bool only_cold_start_class;
  };

 private:
  Options m_options;
};
