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

class ConstantPropagationPass : public Pass {
 public:
  ConstantPropagationPass() : Pass("ConstantPropagationPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    std::vector<std::string> blacklist_names;
    pc.get("blacklist", {}, blacklist_names);
    pc.get("only_simple_branches", false, m_config.only_simple_branches);

    for (auto const& name : blacklist_names) {
      DexType* entry = DexType::get_type(name.c_str());
      if (entry) {
        TRACE(CONSTP, 2, "blacklist class: %s\n", SHOW(entry));
        m_config.blacklist.insert(entry);
      }
    }
  }
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Config {
    std::unordered_set<DexType*> blacklist;
    bool only_simple_branches;
  } m_config;
};
