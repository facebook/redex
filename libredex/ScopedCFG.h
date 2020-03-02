/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
  /** Ensure an editable CFG exists for `code`, creating one if needed. */
  inline explicit ScopedCFG(IRCode* code);

  /** Clears `m_code`'s CFG, if it was created by this RAII instance. */
  inline ~ScopedCFG();

  inline ControlFlowGraph* get();
  inline const ControlFlowGraph* get() const;

  inline ControlFlowGraph& operator*();
  inline const ControlFlowGraph& operator*() const;

  inline ControlFlowGraph* operator->();
  inline const ControlFlowGraph* operator->() const;

 private:
  IRCode* m_code;
  bool m_owns_cfg;
};

ScopedCFG::ScopedCFG(IRCode* code)
    : m_code(code), m_owns_cfg(!code->editable_cfg_built()) {
  if (m_owns_cfg) {
    m_code->build_cfg(/* editable */ true);
  }
}

ScopedCFG::~ScopedCFG() {
  if (m_owns_cfg) {
    m_code->clear_cfg();
  }
}

ControlFlowGraph* ScopedCFG::get() { return &m_code->cfg(); }

const ControlFlowGraph* ScopedCFG::get() const { return &m_code->cfg(); }

ControlFlowGraph& ScopedCFG::operator*() { return m_code->cfg(); }

const ControlFlowGraph& ScopedCFG::operator*() const { return m_code->cfg(); }

ControlFlowGraph* ScopedCFG::operator->() { return &m_code->cfg(); }

const ControlFlowGraph* ScopedCFG::operator->() const { return &m_code->cfg(); }

} // namespace cfg
