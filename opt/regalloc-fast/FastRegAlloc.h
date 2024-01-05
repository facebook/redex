/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class DexMethod;

namespace fastregalloc {

/*
 * This pass implements a Linear Scan register allocator.
 */
class FastRegAllocPass : public Pass {
 public:
  FastRegAllocPass() : Pass("FastRegAllocPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {HasSourceBlocks, Preserves},
    };
  }

  void eval_pass(DexStoresVector& stores,
                 ConfigFiles& conf,
                 PassManager& mgr) override;
  bool is_cfg_legacy() override { return true; }
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_run{0}; // Which iteration of `run_pass`.
  size_t m_eval{0}; // How many `eval_pass` iterations.
};

} // namespace fastregalloc
