/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iostream>

#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "AbstractDomain.h"

namespace sparta {

// Function level analyzer need to extend this class. This class supports the
// static assertion in the InterproceduralAnalyzer. This choice is made so that
// the compiler will throw reasonable errors when the provided function
// analyzers don't implement the necessary methods. We prefer this over template
// errors.

class Intraprocedural {
 public:
  virtual void analyze() = 0;
  virtual void summarize() = 0;

  virtual ~Intraprocedural() {}
};

// Typical Usage:

// struct IRAdaptor /* defined for the IR */ {
//   type Function (an analysis unit),
//   type Program (The data structure that holds functions),
//   type CallGraphInterface (The interface used in fixpoint iterators),
//   function call_graph_of,
// };

// struct Analysis : public IRAdaptor {
//   type Map (may need to be thread-safe for parallel fixpoints),
//   type FunctionSummary (funtion properties that maybe retrived at callsite),
//   type FunctionAnalyzer (A class that extends `Intraprocedural`),
//   type Callsite (Calling context),
//   type MonotonicFixpointIterator (Choice of `MonotonicFixpointIterator`s),
// };

template <typename Analysis>
class InterproceduralAnalyzer {
 public:
  using Function = typename Analysis::Function;
  using Program = typename Analysis::Program;
  using Summary = typename Analysis::FunctionSummary;

  // Map type that accepts 2 type arguments, namely need to support
  // Map<Function, Summary>
  template <typename Key, typename Value>
  using Map = typename Analysis::template Map<Key, Value>;

  using CallGraphInterface = typename Analysis::CallGraphInterface;
  using FunctionAnalyzer =
      typename Analysis::template FunctionAnalyzer<Map<Function, Summary>>;
  using CallGraph = typename CallGraphInterface::Graph;
  using Callsite = typename Analysis::Callsite;
  using CallerContext = typename Callsite::Domain;

  using IntraFn = std::function<std::shared_ptr<FunctionAnalyzer>(
      const Function&, Map<Function, Summary>*, CallerContext*)>;

  // The summary map key doesn't have to be Function
  Map<Function, Summary> function_summaries;

  class CallGraphFixpointIterator final
      : public Analysis::template FixpointIteratorBase<
            CallGraphInterface,
            typename Callsite::Domain> {
   private:
    IntraFn m_intraprocedural;
    Map<Function, Summary>* m_function_summaries;

   public:
    static CallerContext initial_domain() { return CallerContext(); }

    explicit CallGraphFixpointIterator(
        const CallGraph& graph,
        Map<Function, Summary>* function_summaries,
        const IntraFn& intraprocedural)
        : Analysis::template FixpointIteratorBase<CallGraphInterface,
                                                  CallerContext>(graph),
          m_intraprocedural(intraprocedural),
          m_function_summaries(function_summaries) {}

    virtual void analyze_node(const Function& node,
                              CallerContext* current_state) const override {
      m_intraprocedural(node, this->m_function_summaries, current_state)
          ->summarize();
    }

    CallerContext analyze_edge(
        const typename CallGraphInterface::EdgeId&,
        const CallerContext& exit_state_at_source) const override {
      // TODO: Edges have no semantic transformers attached by default. Optional
      // callback?
      return exit_state_at_source;
    }
  };

  virtual ~InterproceduralAnalyzer() {
    // perform static assertions here.
    static_assert(std::is_base_of<AbstractDomain<Map<Function, Summary>>,
                                  Map<Function, Summary>>::value,
                  "Function summary registry should be an abstract domain");

    static_assert(std::is_base_of<Intraprocedural, FunctionAnalyzer>::value,
                  "FunctionAnalyzer must inherit from sparta::Intraprocedural");

    static_assert(
        std::is_base_of<AbstractDomain<CallerContext>, CallerContext>::value,
        "Callsite::Domain must inherit from sparta::AbstractDomain");
  }

  InterproceduralAnalyzer(Program program, int max_iteration)
      : m_program(std::move(program)), m_max_iteration(max_iteration) {}

  virtual std::shared_ptr<CallGraphFixpointIterator> run() {
    // keep a copy of old function summaries, do fixpoint on this level.
    Map<Function, Summary> new_summary = function_summaries;

    std::shared_ptr<CallGraphFixpointIterator> fp = nullptr;

    for (int iteration = 0; iteration < m_max_iteration; iteration++) {
      auto callgraph = Analysis::call_graph_of(m_program, &function_summaries);

      // TODO: remove or abstract logging
      std::cerr << "Iteration " << iteration + 1 << std::endl;

      fp = std::make_shared<CallGraphFixpointIterator>(
          callgraph,
          &new_summary,
          [this](const Function& func,
                 Map<Function, Summary>* summaries,
                 CallerContext* context) -> std::shared_ptr<FunctionAnalyzer> {
            // intraprocedural part
            return this->run_on_function(func, summaries, context);
          });

      // TODO: double check this, I think it actually makes sense to join
      // the caller context domains then use it as the initial domain for
      // the next iteration.
      fp->run(CallGraphFixpointIterator::initial_domain());

      if (new_summary.leq(this->function_summaries) &&
          new_summary != this->function_summaries) {
        // FIXME: This assignment *might* be expensive. However, the good thing
        // is that allocation doesn't always have to happen here.
        function_summaries = new_summary;
      } else {
        std::cerr << "Global fixpoint reached after " << iteration + 1
                  << " iterations." << std::endl;
        break;
      }
    }

    return fp;
  }

  virtual std::shared_ptr<FunctionAnalyzer> run_on_function(
      const Function& function,
      Map<Function, Summary>* summaries,
      CallerContext* context) {

    auto analyzer =
        std::make_shared<FunctionAnalyzer>(function, summaries, context);

    analyzer->analyze();
    return analyzer;
  }

 private:
  Program m_program;
  int m_max_iteration;
};

} // namespace sparta
