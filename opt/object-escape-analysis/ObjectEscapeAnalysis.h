/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

struct ObjectEscapeConfig {
  size_t max_inline_size;
  int max_inline_invokes_iterations;
  int64_t incomplete_estimated_delta_threshold;
  size_t cost_method;
  size_t cost_class;
  size_t cost_field;
  int64_t cost_invoke;
  int64_t cost_move_result;
  int64_t cost_new_instance;
  int64_t savings_threshold;
};

class ObjectEscapeAnalysisPass : public Pass {
 public:
  explicit ObjectEscapeAnalysisPass(bool register_plugins = true);

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        {SpuriousGetClassCallsInterned, RequiresAndPreserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ObjectEscapeConfig m_config;
};
