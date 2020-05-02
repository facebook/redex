/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "IRCode.h"

namespace cfg {

/**
 * RAII abstraction for accessing the Editable ControlFlowGraph for a given
 * IRCode instance.  Will create an editable CFG if one does not already exist,
 * which will be cleared when the instance is cleaned up, whereas an existing
 * editable CFG will remaining after the instance is cleaned up.
 */
class ScopedCFG {
 public:
  ScopedCFG() = default;

  /** Ensure an editable CFG exists for `code`, creating one if needed. */
  inline explicit ScopedCFG(IRCode* code);

  ScopedCFG(const ScopedCFG&) = delete;
  inline ScopedCFG(ScopedCFG&& rhs) noexcept;

  /** Clears `m_code`'s CFG, if it was created by this RAII instance. */
  inline ~ScopedCFG();

  ScopedCFG& operator=(const ScopedCFG&) = delete;
  inline ScopedCFG& operator=(ScopedCFG&& rhs) noexcept;

  inline ControlFlowGraph* get();
  inline const ControlFlowGraph* get() const;

  inline ControlFlowGraph& operator*();
  inline const ControlFlowGraph& operator*() const;

  inline ControlFlowGraph* operator->();
  inline const ControlFlowGraph* operator->() const;

 private:
  inline void reset();
  inline void reset(IRCode* code, bool owns_cfg);

  IRCode* m_code{nullptr};
  bool m_owns_cfg{false};
};

ScopedCFG::ScopedCFG(IRCode* code)
    : m_code(code), m_owns_cfg(!code->editable_cfg_built()) {
  if (m_owns_cfg) {
    m_code->build_cfg(/* editable */ true);
  }
}

ScopedCFG::ScopedCFG(ScopedCFG&& rhs) noexcept {
  reset(rhs.m_code, rhs.m_owns_cfg);
  rhs.m_code = nullptr;
  rhs.m_owns_cfg = false;
};

ScopedCFG::~ScopedCFG() { reset(); }

ScopedCFG& ScopedCFG::operator=(ScopedCFG&& rhs) noexcept {
  if (this == &rhs) {
    return *this;
  }
  reset(rhs.m_code, rhs.m_owns_cfg);
  rhs.m_code = nullptr;
  rhs.m_owns_cfg = false;
  return *this;
}

void ScopedCFG::reset() { reset(nullptr, false); }

void ScopedCFG::reset(IRCode* code, bool owns_cfg) {
  if (m_owns_cfg) {
    m_code->clear_cfg();
  }
  m_code = code;
  m_owns_cfg = owns_cfg;
  redex_assert(code == nullptr || !m_owns_cfg || code->cfg_built());
}

ControlFlowGraph* ScopedCFG::get() { return &m_code->cfg(); }

const ControlFlowGraph* ScopedCFG::get() const { return &m_code->cfg(); }

ControlFlowGraph& ScopedCFG::operator*() { return m_code->cfg(); }

const ControlFlowGraph& ScopedCFG::operator*() const { return m_code->cfg(); }

ControlFlowGraph* ScopedCFG::operator->() { return &m_code->cfg(); }

const ControlFlowGraph* ScopedCFG::operator->() const { return &m_code->cfg(); }

} // namespace cfg
