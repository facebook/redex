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

  void configure_pass(const JsonWrapper& jw) override {
    jw.get("instrumentation_strategy", "", m_options.instrumentation_strategy);
    jw.get("analysis_class_name", "", m_options.analysis_class_name);
    jw.get("analysis_method_name", "", m_options.analysis_method_name);
    std::vector<std::string> list;
    jw.get("blacklist", {}, list);
    for (const auto& e : list) {
      m_options.blacklist.insert(e);
    }
    jw.get("whitelist", {}, list);
    for (const auto& e : list) {
      m_options.whitelist.insert(e);
    }
    jw.get("blacklist_file_name", "", m_options.blacklist_file_name);
    jw.get("metadata_file_name", "instrument-mapping.txt",
           m_options.metadata_file_name);
    jw.get("num_stats_per_method", 1, m_options.num_stats_per_method);
    jw.get("only_cold_start_class", true, m_options.only_cold_start_class);
  }

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
