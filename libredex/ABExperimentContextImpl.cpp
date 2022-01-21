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

// Default is test
ABExperimentState s_default_state{ABExperimentState::TEST};
} // namespace

void ABExperimentContextImpl::flush() {
  if (m_flushed) {
    return;
  }

  always_assert_log(use_test(), "Should not flush unless in test mode.");
}

ABExperimentContextImpl::ABExperimentContextImpl(const std::string& exp_name) {
  if (s_experiments_states.count(exp_name) == 0) {
    // if default is not passed as a config, default falls back to test
    s_experiments_states[exp_name] = s_default_state;
  }
  m_state = s_experiments_states[exp_name];
  ++INST_CNT;
}

void ABExperimentContextImpl::parse_experiments_states(ConfigFiles& conf) {
  always_assert(INST_CNT == 0);
  always_assert_log(s_experiments_states.empty(),
                    "Cannot set the experiments states map more than once");

  auto& json_conf = conf.get_json_config();

  std::unordered_map<std::string, std::string> exp_states;
  json_conf.get("ab_experiments_states", {}, exp_states);
  {
    std::unordered_map<std::string, std::string> exp_states_override;
    json_conf.get("ab_experiments_states_override", {}, exp_states_override);
    for (auto& p : exp_states_override) {
      exp_states[p.first] = std::move(p.second);
    }
  }

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

  for (auto& it : exp_states) {
    s_experiments_states[it.first] = transform_state(it.first, it.second);
  }

  if (json_conf.contains("ab_experiments_default")) {
    s_default_state = transform_state(
        "default", json_conf.get("ab_experiments_default", std::string("")));
  }
}

std::unordered_set<std::string>
ABExperimentContextImpl::get_all_experiments_names() {
  std::unordered_set<std::string> exp_names;
  for (auto&& [name, _] : s_experiments_states) {
    exp_names.insert(name);
  }
  return exp_names;
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
