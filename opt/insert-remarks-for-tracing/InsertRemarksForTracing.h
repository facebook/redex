/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

// A validation-only pass that stamps a redex-internal Remark MIE at the head of
// every block, tagging each with this pass as `producer`, the deobfuscated
// method name as `val_str`, and the block id as `val_int`. It exists to
// stress-test the Remark plumbing end-to-end: after this pass runs, the
// per-pass remarks_count metric (emitted by PassManager) lets us observe how
// many remarks survive each subsequent optimization pass on a real app. A
// collapse to zero indicates a broken carry (deep_copy / split / fold); the
// final count must be zero only after IRCode::sync strips them.
//
// Gated on the global `insert_remarks` flag (GlobalConfig): run_pass is a no-op
// unless the flag is set, so the pass is inert by default even when it is
// present in the pipeline. Enable the flag only for integration testing.
class InsertRemarksForTracingPass : public Pass {
 public:
  InsertRemarksForTracingPass() : Pass("InsertRemarksForTracingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    // Inserting block-anchored, redex-internal, strippable metadata does not
    // affect any code property.
    return redex_properties::simple::preserves_all();
  }

  std::string get_config_doc() override {
    return trim(R"(
A validation-only pass that inserts a Remark MIE at the head of every block, to
exercise the Remark plumbing across the pipeline. No-op unless the global
insert_remarks flag is set.
    )");
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
