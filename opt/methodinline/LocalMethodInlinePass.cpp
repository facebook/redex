/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass is a variation of the inliner that makes all inlining decisions
 * only with local call-site specific considerations, effectively disabling the
 * global considerations, where the inliner might inline a method everywhere
 * when that would reduce DEX size by eliminating the callee method, even though
 * each individual call-site might grow a bit in size.
 *
 * By only doing local decisions, the end result is that every call-site can
 * only become smaller in size, never bigger.
 *
 * While the inliner in general operates in a bottom-up approach, making all
 * call-sites smaller means that no inlining opportunities at the top will
 * become too costly to be inlined, which may happen when the inliner is allowed
 * to take into account global considerations. In effect, by running this pass
 * first we can some of the benefits you would hope for in a top-down inlining
 * approach.
 */

#include "LocalMethodInlinePass.h"

#include "MethodInliner.h"

void LocalMethodInlinePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf, DEFAULT_COST_CONFIG,
                       /* intra_dex */ false,
                       /* inline_for_speed */ nullptr,
                       /* inline_bridge_synth_only */ false,
                       /* local_only */ true);
}

static LocalMethodInlinePass s_pass;
