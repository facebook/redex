/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

#include "ApiLevelsUtils.h"

/**
 * A method reference encoded in an invoke-virtual/interface instruction can be
 * adjusted or rebound as long as it can be resolved to the correct method
 * definition at runtime. Since method reference count is usually the first
 * limit we hit when emitting a dex file, we can reduce the number of unique
 * method references by playing with how we bind method reference at virtual
 * call sites. This is what RebindRefsPass does.
 *
 * We want to reduce the number of unique method reference
 * we emit in the final dex code, but at the same time we shouldn't slowdown
 * performance critical code.
 *
 * Note that we should run this pass later on in the pipeline after we've
 * removed the unreachable code. Generalizing method references will expand our
 * call-graph statically. Including more code is an undesired side-affect of
 * running this pass too early.
 */
class ReBindRefsPass : public Pass {
 public:
  ReBindRefsPass() : Pass("ReBindRefsPass") {}

  void bind_config() override {
    // Allowing resolving method ref to an external one.
    bind("rebind_to_external", false, m_rebind_to_external);
    bind("excluded_externals", {}, m_excluded_externals,
         "Externals types/prefixes excluded from reference rebinding");
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  bool m_rebind_to_external;
  std::vector<std::string> m_excluded_externals;
  const api::AndroidSDK* m_min_sdk_api;
};
