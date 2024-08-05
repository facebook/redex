/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantPropagationState.h"
#include "ConstantPropagationTransform.h"
#include "IRCode.h"

namespace constant_propagation {

struct Config {
  Transform::Config transform;
};

class ConstantPropagation final {
 public:
  explicit ConstantPropagation(const Config& config) : m_config(config) {}

  Transform::Stats run(DexMethod* method,
                       const XStoreRefs* xstores,
                       const State& state);
  Transform::Stats run(const Scope& scope,
                       const XStoreRefs* xstores,
                       const State& state);

 private:
  const Config& m_config;
};
} // namespace constant_propagation
