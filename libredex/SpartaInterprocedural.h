/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Analyzer.h"
#include "CallGraph.h"
#include "DexClass.h"
#include "MonotonicFixpointIterator.h"

namespace sparta_interprocedural {

struct AnalysisAdaptorBase {
  using Function = const DexMethod*;
  using Program = const Scope&;
  using CallGraphInterface = call_graph::GraphInterface;

  // Uses the serial fixpoint iterator by default. The user can override this
  // type alias to use the parallel fixpoint.
  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::MonotonicFixpointIterator<GraphInterface, Domain>;

  // The summary argument is unused in the adaptor base. Only certain
  // analyses will require this argument, in which case this function
  // should be *overriden* in the derived class.
  template <typename FunctionSummaries>
  static call_graph::Graph call_graph_of(const Scope& scope,
                                         FunctionSummaries* /*summaries*/) {
    // TODO: build method override graph and merge them together?
    // TODO: build once and cache it in the memory because the framework
    // will call it on every top level iteration.
    return call_graph::single_callee_graph(scope);
  }
};

} // namespace sparta_interprocedural
