/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>

#include "Pass.h"
#include "PassManager.h"

/*
 * Implementation notes; see get_config_doc() for the user-facing purpose.
 *
 * The reservation is taken in `eval_pass` (which runs before any pass's
 * `run_pass`, hence before `InterDexPass`), so InterDex and
 * `InterDexReshufflePass` leave the slot free; it is released in `run_pass`. It
 * is taken only when `enable_replacing_areequal` is set, so a build that does
 * not run the rewrite pays no headroom cost.
 *
 * Scheduling and presence are enforced by the `NeedsAreEqualRefReservation`
 * (Negative) property: every constant-propagation pass `Establishes` it and
 * this pass `Destroys` it. A `Negative` property must be unset at the end, so a
 * pipeline that runs constant propagation without this pass clearing it
 * afterward -- this pass missing, or a constant-propagation pass scheduled
 * after it -- fails the pass-order check.
 */
class ReserveAreEqualRefPass : public Pass {
 public:
  ReserveAreEqualRefPass() : Pass("ReserveAreEqualRefPass") {}

  std::string get_config_doc() override {
    return trim(R"(
Reserves one method ref so the Kotlin `Intrinsics.areEqual` -> `Object.equals`
rewrite can introduce its `Object.equals` reference even after InterDex has
packed the dexes, without pushing any dex past the per-dex method ref limit. The
rewrite adds at most one new ref (`Object.equals`) per dex no matter how many
passes perform it, so a single reserved ref is enough headroom. This pass must
run after the last pass that performs the rewrite -- the last pass that runs
constant propagation, directly or via the Shrinker.
)");
  }

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    // This pass never modifies the IR; it only reserves a method ref in
    // `eval_pass` and releases it in `run_pass`. So it preserves every
    // property, with the sole exception of the bookkeeping
    // `NeedsAreEqualRefReservation` it exists to clear.
    auto interactions = redex_properties::simple::preserves_all();
    interactions.at(NeedsAreEqualRefReservation) = Destroys;
    return interactions;
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
