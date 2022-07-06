/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

class CallGraphFileGenerationPass : Pass {
 public:
  CallGraphFileGenerationPass() : Pass("CallGraphFileGenerationPass") {}

  void bind_config() override;
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  const char* CALL_GRAPH_FILE = "redex-callgraph.graph";
  bool m_emit_graph{false};
};
