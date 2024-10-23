/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class WriteBarrierLoweringPass : public Pass {
 public:
  WriteBarrierLoweringPass() : Pass("WriteBarrierLoweringPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoWriteBarrierInstructions, Establishes},
        {NoUnreachableInstructions, Preserves},
        {RenameClass, Preserves},
        {DexLimitsObeyed, Preserves},
        {NoInitClassInstructions, Preserves},
    };
  }

  void eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
