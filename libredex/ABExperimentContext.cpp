/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ABExperimentContext.h"
#include "ABExperimentContextImpl.h"
#include "ControlFlow.h"

using namespace ab_test;

std::unique_ptr<ABExperimentContext> ABExperimentContext::create(
    cfg::ControlFlowGraph* cfg,
    DexMethod* m,
    const std::string& exp_name,
    ABExperimentPreferredMode ab_experiment_mode) {
  return std::make_unique<ABExperimentContextImpl>(
      cfg, m, exp_name, ab_experiment_mode);
}

void ABExperimentContext::parse_experiments_states(
    const std::unordered_map<std::string, std::string>& states) {
  ABExperimentContextImpl::parse_experiments_states(states);
}

void ABExperimentContext::force_test_mode() {
  ABExperimentContextImpl::set_global_mode(ABGlobalMode::TEST);
}

void ABExperimentContext::force_control_mode() {
  ABExperimentContextImpl::set_global_mode(ABGlobalMode::CONTROL);
}

void ABExperimentContext::disable_ab_experiments() {
  ABExperimentContextImpl::set_global_mode();
}
