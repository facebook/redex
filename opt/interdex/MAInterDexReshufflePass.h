/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "InterDex.h"
#include "InterDexReshuffleImpl.h"
#include "Pass.h"

/* Similar to InterDexReshufflePass, this pass impls Local Search Algotithm to
 * minize cross-dex refs by reshuffling classes among dex files. Different from
 * InterDexReshufflePass, when reshuffling classes, this pass considers the
 * classes mergeability. That is, if two classes may be merged in later IDCM,
 * they have the high possibility to moved to the same dex.
 */
class MergeabilityAwareInterDexReshufflePass : public Pass {
 public:
  explicit MergeabilityAwareInterDexReshufflePass()
      : Pass("MergeabilityAwareInterDexReshufflePass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void bind_config() override {
    bind("reserved_extra_frefs",
         m_config.reserved_extra_frefs,
         m_config.reserved_extra_frefs,
         "How many extra frefs to be reserved for the dexes this pass "
         "processes.");
    bind("reserved_extra_trefs",
         m_config.reserved_extra_trefs,
         m_config.reserved_extra_trefs,
         "How many extra trefs to be reserved for the dexes this pass "
         "processes.");
    bind("reserved_extra_mrefs",
         m_config.reserved_extra_mrefs,
         m_config.reserved_extra_mrefs,
         "How many extra mrefs to be reserved for the dexes this pass "
         "processes.");
    bind("extra_linear_alloc_limit",
         m_config.extra_linear_alloc_limit,
         m_config.extra_linear_alloc_limit,
         "How many extra linear_alloc_limit to be reserved for the dexes "
         "this pass rocesses.");
    bind("max_batches",
         m_config.max_batches,
         m_config.max_batches,
         "How many batches to execute. More might yield better results, but "
         "might take longer.");
    bind("max_batch_size",
         m_config.max_batch_size,
         m_config.max_batch_size,
         "How many class to move per batch. More might yield better results, "
         "but might take longer.");
    bind("other_weight",
         m_config.other_weight,
         m_config.other_weight,
         "Weight for non-deduped method in mergeability-aware reshuffle cost "
         "function.");
    bind("deduped_weight",
         m_config.deduped_weight,
         m_config.deduped_weight,
         "Weight for deduped method in mergeability-aware reshuffle cost "
         "function.");
    bind("exclude_below20pct_coldstart_classes",
         false,
         m_config.exclude_below20pct_coldstart_classes,
         "Whether to exclude coldstart classes in between 1pctColdStart and "
         "20pctColdStart marker"
         "from the reshuffle.");
  }

 private:
  ReshuffleConfig m_config;
};
