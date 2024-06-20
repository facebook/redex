/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagationTransform.h"
#include "IRCode.h"

namespace constant_propagation {

struct Config {
  Transform::Config transform;
};

class ConstantPropagation final {
 public:
  explicit ConstantPropagation(
      const Config& config,
      int min_sdk = 0,
      constant_propagation::ImmutableAttributeAnalyzerState*
          immut_analyzer_state = nullptr)
      : m_config(config),
        m_min_sdk(min_sdk),
        m_immut_analyzer_state(immut_analyzer_state) {}

  Transform::Stats run(DexMethod* method, XStoreRefs* xstores);
  Transform::Stats run(DexMethod* method,
                       XStoreRefs* xstores,
                       const Transform::RuntimeCache& runtime_cache);
  Transform::Stats run(const Scope& scope, XStoreRefs* xstores);

 private:
  const Config& m_config;
  int m_min_sdk;
  constant_propagation::ImmutableAttributeAnalyzerState* m_immut_analyzer_state;
};
} // namespace constant_propagation
