/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ControlFlow.h"

ControlFlowGraph::~ControlFlowGraph() {
  for (auto block : m_blocks) {
    delete block;
  }
}

void ControlFlowGraph::add_edge(Block* p, Block* s, EdgeType type) {
  if (std::find(p->succs().begin(), p->succs().end(), s) == p->succs().end()) {
    p->succs().push_back(s);
    s->preds().push_back(p);
  }
  edge(p, s).set(type);
}

void ControlFlowGraph::remove_edge(Block* p, Block* s, EdgeType type) {
  edge(p, s).reset(type);
  if (edge(p, s).none()) {
    remove_all_edges(p, s);
  }
}

void ControlFlowGraph::remove_all_edges(Block* p, Block* s) {
  edge(p, s).reset();
  p->succs().erase(std::remove_if(p->succs().begin(),
                                  p->succs().end(),
                                  [&](Block* b) { return b == s; }),
                   p->succs().end());
  s->preds().erase(std::remove_if(s->preds().begin(),
                                  s->preds().end(),
                                  [&](Block* b) { return b == p; }),
                   s->preds().end());
}
