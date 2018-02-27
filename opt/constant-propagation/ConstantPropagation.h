/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Pass.h"

class ConstantPropagationPass : public Pass {
 public:
  struct Config {
    constant_propagation::intraprocedural::FixpointIterator::Config
        intraprocedural_analysis;
    constant_propagation::Transform::Config transform;
  };

  ConstantPropagationPass() : Pass("ConstantPropagationPass") {}

  virtual void configure_pass(const PassConfig& pc) override;
  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& cfg,
                        PassManager& mgr) override;

 private:
  Config m_config;
};
