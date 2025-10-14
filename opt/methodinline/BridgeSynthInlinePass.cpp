/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BridgeSynthInlinePass.h"

#include "MethodInliner.h"

void BridgeSynthInlinePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  inliner::run_inliner(stores,
                       mgr,
                       conf,
                       DEFAULT_COST_CONFIG,
                       HotColdInliningBehavior::None,
                       /* partial_hot_hot */ false,
                       /* intra_dex */ false,
                       /* baseline_profile_guided */ false,
                       /* inline_for_speed */ nullptr,
                       /* inline_bridge_synth_only */ true);
}

static BridgeSynthInlinePass s_pass;
