/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "PassManager.h"

#include <unordered_set>

class InstrumentPass : public Pass {
 public:
  InstrumentPass() : Pass("InstrumentPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    pc.get("analysis_class_name", "", m_analysis_class_name);
    pc.get("onMethodBegin_name", "", m_onMethodBegin_name);
    pc.get("num_stats_per_method", 1, m_num_stats_per_method);
    pc.get("method_index_file_name", "instrument-methods-idx.txt",
           m_method_index_file_name);

    std::vector<std::string> list;
    pc.get("blacklist", {}, list);
    for (const auto& e : list) {
      m_blacklist.insert(e);
    }
    pc.get("whitelist", {}, list);
    for (const auto& i : list) {
      m_whitelist.insert(i);
    }
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::string m_analysis_class_name;
  std::string m_onMethodBegin_name;
  int64_t m_num_stats_per_method;
  std::string m_method_index_file_name;
  std::unordered_set<std::string> m_blacklist;
  std::unordered_set<std::string> m_whitelist;
};
