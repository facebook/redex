/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ExternalRefsManglingPass.h"

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
class ReBindRefsPass : public ExternalRefsManglingPass {
 public:
  ReBindRefsPass() : ExternalRefsManglingPass("ReBindRefsPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoSpuriousGetClassCalls, Preserves},
    };
  }

  void bind_config() override { ExternalRefsManglingPass::bind_config(); }

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override {
    ExternalRefsManglingPass::eval_pass(stores, conf, mgr);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
