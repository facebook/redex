/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ABExperimentContextImpl.h"
#include "ControlFlow.h"

using namespace ab_test;

enum class ABExperimentState { CONTROL, TEST };

namespace {
// Counter for the number of existing experiment context instances.
// Used to make sure the global mode cannot be changed while at least one
// experiment context instance exists.
uint16_t INST_CNT{0};

std::unordered_map<std::string, ABExperimentState> s_experiments_states;
boost::optional<ABExperimentState> s_default_state;
} // namespace

void ABExperimentContextImpl::flush() {
  if (m_flushed) {
    return;
  }

  always_assert_log(use_test(), "Should not flush unless in test mode.");
}

ABExperimentContextImpl::ABExperimentContextImpl(const std::string& exp_name) {
  m_state = s_experiments_states.count(exp_name) != 0
                ? s_experiments_states[exp_name]
            : s_default_state ? *s_default_state
                              : ABExperimentState::TEST;

  ++INST_CNT;
}

void ABExperimentContextImpl::parse_experiments_states(
    const std::unordered_map<std::string, std::string>& states,
    const boost::optional<std::string>& default_state) {
  always_assert(INST_CNT == 0);
  always_assert_log(s_experiments_states.empty(),
                    "Cannot set the experiments states map more than once");

  const auto transform_state = [](const std::string& exp,
                                  const std::string& state) {
    if (state == "control") {
      return ABExperimentState::CONTROL;
    } else if (state == "test") {
      return ABExperimentState::TEST;
    } else {
      not_reached_log("Unknown AB Experiment state %s", exp.c_str());
    }
  };

  for (auto& it : states) {
    s_experiments_states[it.first] = transform_state(it.first, it.second);
  }

  s_default_state = default_state
                        ? boost::optional<ABExperimentState>(
                              transform_state("default", *default_state))
                        : boost::none;
}

bool ABExperimentContextImpl::use_test() {
  return m_state == ABExperimentState::TEST;
}

bool ABExperimentContextImpl::use_control() {
  return m_state == ABExperimentState::CONTROL;
}

void ABExperimentContextImpl::reset_global_state() {
  s_experiments_states.clear();
  INST_CNT = 0;
}
