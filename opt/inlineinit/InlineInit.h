/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

  void configure_pass(const JsonWrapper& jw) override {
    std::vector<std::string> black_list;
    jw.get("class_black_list", {}, black_list);
    for (const auto& type_s : black_list) {
      m_inliner_config.caller_black_list.emplace(
          DexType::make_type(type_s.c_str()));
    }
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
