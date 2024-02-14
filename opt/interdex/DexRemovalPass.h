/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConfigFiles.h"
#include "DexClass.h"
#include "Pass.h"
class DexRemovalPass : public Pass {
 public:
  explicit DexRemovalPass() : Pass("DexRemovalPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override {
    bind("class_reshuffle", false, m_class_reshuffle);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  // By default, it is false. True means DexRemovalPass will use interdex
  // reshuffle algorithm to guide dex removal. False means this pass only
  // removes current empty dexes if there is any.

  bool m_class_reshuffle;

  // It is possible that after previous optimizations passes, there are dexes
  // which already become empty. Remove those dexes if there is any.
  size_t remove_empty_dexes(DexClassesVector& dexen);

  // Once any dex is removed, check if 1) any original classes are missing; 2)
  // cannary classes are in good shape.
  void sanity_check(Scope& original_scope,
                    DexStoresVector& stores,
                    size_t removed_num_dexes);
};
