/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ABExperimentContextImpl.h"
#include "ControlFlow.h"

using namespace ab_test;

namespace {
// True if an ABExperimentContext instance has already been created. Used to
// make sure AB_GLOBAL_MODE is not modified after such an instance is created
// to ensure that all instances will use the same global mode.
static bool AB_INSTANTIATED = false;

// True if the global mode has already been set/used. Used to make sure
// AB_GLOBAL_MODE is not modified after it has been used to ensure that force_*
// methods are used at MOST once.
static bool AB_GLOBAL_INSTANTIATED = false;

// Optional global mode, NONE falls back to the preferred_mode of each
// experiment context instance.
static ABGlobalMode AB_GLOBAL_MODE = ABGlobalMode::NONE;
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

bool ABExperimentContextImpl::use_test() {
  return AB_GLOBAL_MODE == ABGlobalMode::TEST ||
         (AB_GLOBAL_MODE == ABGlobalMode::NONE &&
          m_preferred_mode == ABExperimentPreferredMode::PREFER_TEST);
}

void ABExperimentContextImpl::initialize_global_mode(
    ABGlobalMode ab_global_mode) {
  // no change to initial value
  always_assert(!AB_INSTANTIATED);
  always_assert(!AB_GLOBAL_INSTANTIATED);
  AB_GLOBAL_INSTANTIATED = true;
  AB_GLOBAL_MODE = ab_global_mode;
}

void ABExperimentContextImpl::setup_context() {
  always_assert(m_cfg->editable());
  AB_INSTANTIATED = true;

  if (use_test()) {
    return;
  }

  // Clone the original content of the CFG
  m_cloned_cfg = std::make_unique<cfg::ControlFlowGraph>();
  m_cfg->deep_copy(m_cloned_cfg.get());
}
