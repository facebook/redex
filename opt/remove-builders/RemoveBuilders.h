/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class RemoveBuildersPass : public Pass {
 public:
  RemoveBuildersPass() : Pass("RemoveBuildersPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}}};
  }

  void bind_config() override {
    bind("enable_buildee_constr_change", false, m_enable_buildee_constr_change);
    bind("blocklist", {}, m_blocklist);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::unordered_set<DexType*> m_builders;
  std::unordered_set<DexType*> m_blocklist;
  bool m_enable_buildee_constr_change;

  std::vector<DexType*> created_builders(DexMethod*);
  bool escapes_stack(DexType*, DexMethod*);
};
