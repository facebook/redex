/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GraphColoring.h"
#include "Pass.h"

class DexMethod;

namespace regalloc {

class RegAllocPass : public Pass {
 public:
  RegAllocPass() : Pass("RegAllocPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{NoInitClassInstructions, {.preserves = true}}};
  }

  void bind_config() override {
    bool unused;
    bind("live_range_splitting", false, unused);
    trait(Traits::Pass::atleast, 1);
  }

  bool is_editable_cfg_friendly() override { return true; }

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_run{0}; // Which iteration of `run_pass`.
  size_t m_eval{0}; // How many `eval_pass` iterations.
};

} // namespace regalloc
