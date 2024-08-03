/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "PassManager.h"

class NopperPass : public Pass {
 public:
  NopperPass() : Pass("NopperPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoInitClassInstructions, Preserves},
        {NoResolvablePureRefs, Preserves},
        {NoUnreachableInstructions, Preserves},
        {RenameClass, Preserves},
    };
  }

  void bind_config() override {
    bind("probability", 0.0f, m_probability);
    bind("complex", false, m_complex);
  }

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  float m_probability{0}; // How many percent of blocks to infect
  bool m_complex{false};
  std::optional<ReserveRefsInfoHandle> m_reserved_refs_handle;
};
