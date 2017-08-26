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

class DedupBlocksPass : public Pass {
 public:
  DedupBlocksPass() : Pass("DedupBlocksPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual void configure_pass(const PassConfig& pc) override {
    std::vector<std::string> method_black_list_names;
    pc.get("method_black_list", {}, method_black_list_names);
    for (std::string name : method_black_list_names) {
      auto meth = DexMethod::get_method(name);
      if (meth == nullptr || !meth->is_def()) continue;
      m_config.method_black_list.emplace(static_cast<DexMethod*>(meth));
    }
  }

  struct Config {
    std::unordered_set<DexMethod*> method_black_list;
  } m_config;
};
