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
#include "MethodOverrideGraph.h"
#include "MonotonicFixpointIterator.h"

namespace sparta_interprocedural {

struct AnalysisAdaptorBase {
  using Function = const DexMethod*;
  using Program = Scope;
  using CallGraphInterface = call_graph::GraphInterface;

  // Uses the serial fixpoint iterator by default. The user can override this
  // type alias to use the parallel fixpoint.
  template <typename GraphInterface, typename Domain>
  using FixpointIteratorBase =
      sparta::MonotonicFixpointIterator<GraphInterface, Domain>;

  // The summary argument is unused in the adaptor base. Only certain
  // analyses will require this argument, in which case this function
  // should be *overriden* in the derived class.
  template <typename Registry>
  static call_graph::Graph call_graph_of(const Scope& scope,
                                         Registry* /*reg*/) {
    constexpr uint32_t big_override_threshold = 5;
    return call_graph::multiple_callee_graph(
        *method_override_graph::build_graph(scope),
        scope,
        big_override_threshold);
  }

  static const DexMethod* function_by_node_id(const call_graph::NodeId& node) {
    return node->method();
  }
};

struct BottomUpAnalysisAdaptorBase : public AnalysisAdaptorBase {
  using CallGraphInterface =
      sparta::BackwardsFixpointIterationAdaptor<call_graph::GraphInterface>;
};

template <typename Summary>
class MethodSummaryRegistry : public sparta::AbstractRegistry {
 private:
  ConcurrentMap<const DexMethod*, Summary> m_map;
  bool m_has_update = false;

 public:
  bool has_update() const override { return m_has_update; }
  void materialize_update() override { m_has_update = false; }

  Summary get(const DexMethod* method, Summary default_value) const {
    return m_map.get(method, default_value);
  }

  // returns true if the entry exists.
  bool update(const DexMethod* method,
              std::function<Summary(const Summary&)> updater) {
    bool entry_exists;
    m_map.update(method, [&](const DexMethod*, Summary& value, bool exists) {
      entry_exists = exists;
      value = updater(value);
    });
    m_has_update = true; // benign race conditions as long as materialize_update
                         // is not called during update.
    return entry_exists;
  }

  // `updater` returns true if a change is made in the entry
  void maybe_update(const DexMethod* method,
                    std::function<bool(Summary&)> updater) {
    bool changed = false;
    m_map.update(method,
                 [&](const DexMethod*, Summary& value, bool /* exists */) {
                   changed = updater(value);
                 });

    if (changed) {
      m_has_update = true; // benign race conditions as long as
                           // materialize_update is not called during update.
    }
  }

  // Not thread-safe
  const ConcurrentMap<const DexMethod*, Summary>& get_map() const {
    return m_map;
  }
};

} // namespace sparta_interprocedural
