/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Inliner.h"
#include "Pass.h"
#include "Resolver.h"

class InlineInitPass : public Pass {
  MethodRefCache m_resolved_refs;
  std::unordered_set<DexMethod*> gather_init_candidates(
      const Scope& scope, const DexClasses& primary_dex);

  MultiMethodInliner::Config m_inliner_config;

 public:
  InlineInitPass() : Pass("InlineInitPass") {}

  virtual void configure_pass(const PassConfig& pc) override {
    std::vector<std::string> black_list;
    pc.get("class_black_list", {}, black_list);
    for (const auto& type_s : black_list) {
      m_inliner_config.caller_black_list.emplace(
          DexType::make_type(type_s.c_str()));
    }
  }

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
