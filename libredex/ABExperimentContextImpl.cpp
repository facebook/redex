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

// Optional global mode, NONE falls back to the preferred_mode of each
// experiment context instance.
ABGlobalMode AB_GLOBAL_MODE = ABGlobalMode::NONE;

std::unordered_map<std::string, ABExperimentState> s_experiments_states;
} // namespace

void ABExperimentContextImpl::flush() {
  if (m_flushed) {
    return;
  }
  m_flushed = true;

  if (!use_test()) {
    // control should only keep the original cfg, not the modified one
    m_cloned_cfg->deep_copy(m_cfg);
  } // else do nothing

  // Clean up
  m_original_method->get_code()->clear_cfg();
  --INST_CNT;
}

ABExperimentContextImpl::ABExperimentContextImpl(
    cfg::ControlFlowGraph* cfg,
    DexMethod* m,
    ABExperimentPreferredMode preferred_mode)
    : m_original_method(m), m_cfg(cfg), m_preferred_mode(preferred_mode) {
  always_assert(cfg == &m->get_code()->cfg());
  setup_context();
}

ABExperimentContextImpl::~ABExperimentContextImpl() { flush(); }

void ABExperimentContextImpl::parse_experiments_states(
    const std::unordered_map<std::string, std::string>& states) {
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
}

bool ABExperimentContextImpl::use_test() {
  return AB_GLOBAL_MODE == ABGlobalMode::TEST ||
         (AB_GLOBAL_MODE == ABGlobalMode::NONE &&
          m_preferred_mode == ABExperimentPreferredMode::PREFER_TEST);
}

void ABExperimentContextImpl::set_global_mode(ABGlobalMode ab_global_mode) {
  if (AB_GLOBAL_MODE == ab_global_mode) {
    return;
  }
  // no currently existing instance
  always_assert(INST_CNT == 0);
  AB_GLOBAL_MODE = ab_global_mode;
}

void ABExperimentContextImpl::setup_context() {
  always_assert(m_cfg->editable());
  ++INST_CNT;

  if (use_test()) {
    return;
  }

  // Clone the original content of the CFG
  m_cloned_cfg = std::make_unique<cfg::ControlFlowGraph>();
  m_cfg->deep_copy(m_cloned_cfg.get());
}
