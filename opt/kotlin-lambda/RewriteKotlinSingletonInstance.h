/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "DexUtil.h"
#include "KotlinInstanceRewriter.h"
#include "Pass.h"

// This pass Removes INSTANCE usage in Kotlin singletons with INSTANCE.
// Instance setup in <clinit>
// <clinit>:()V
// new-instance v0, LKDexbolt$main$1;
// invoke-direct {v0}, LKDexbolt$main$1;.<init>:()V
// sput-object v0, LKDexbolt$main$1;.INSTANCE:LKDexbolt$main$1;
// return-void
//
// And the INSTANCE reuse will be:
// sget-object v3, LKDexbolt$main$1;.INSTANCE:LKDexbolt$main$1;
// check-cast v3, Lkotlin/jvm/functions/Function2;
// invoke-virtual {v2, v3},
// LKDexbolt;.doCalc:(Lkotlin/jvm/functions/Function2;)J
//
// https://fburl.com/dexbolt/43t27was
//
// This pass removes the INSTANCE use so that Redex optimisations can optimize
// them better.
// Here the object stored in INSTANCE is not semantically relevant and can be
// moved.
//
class RewriteKotlinSingletonInstance : public Pass {

 public:
  RewriteKotlinSingletonInstance()
      : Pass("RewriteKotlinSingletonInstancePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
        {NoResolvablePureRefs, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
