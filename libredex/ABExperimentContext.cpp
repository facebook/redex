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
    const std::string& exp_name) {
  return std::make_unique<ABExperimentContextImpl>(exp_name);
}

void ABExperimentContext::parse_experiments_states(
    const std::unordered_map<std::string, std::string>& states,
    bool /* unused */) {
  ABExperimentContextImpl::parse_experiments_states(states);
}

void ABExperimentContext::reset_global_state() {
  ABExperimentContextImpl::reset_global_state();
}
