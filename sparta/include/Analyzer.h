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

class AbstractRegistry {
 public:
  virtual bool has_update() const = 0;
  virtual void materialize_update() = 0;

  virtual ~AbstractRegistry() {}
};

// Typical Usage:

// struct IRAdaptor /* defined for the IR */ {
//   type Function (an analysis unit),
//   type Program (The data structure that holds functions),
//   type CallGraphInterface (The interface used in fixpoint iterators),
//   function call_graph_of,
// };

// struct Analysis : public IRAdaptor {
//   type Registry (Function Summaries)
//   type FunctionAnalyzer (A class that extends `Intraprocedural`),
//   type Callsite (Calling context),
//   type MonotonicFixpointIterator (Choice of `MonotonicFixpointIterator`s),
// };

template <typename Analysis>
class InterproceduralAnalyzer {
 public:
  using Function = typename Analysis::Function;
  using Program = typename Analysis::Program;
  using Registry = typename Analysis::Registry;

  using CallGraphInterface = typename Analysis::CallGraphInterface;

  using FunctionAnalyzer = typename Analysis::FunctionAnalyzer;
  using CallGraph = typename CallGraphInterface::Graph;
  using Callsite = typename Analysis::Callsite;
  using CallerContext = typename Callsite::Domain;

  using IntraFn = std::function<std::shared_ptr<FunctionAnalyzer>(
      const Function&, Registry*, CallerContext*)>;

  Registry registry;

  class CallGraphFixpointIterator final
      : public Analysis::template FixpointIteratorBase<
            CallGraphInterface,
            typename Callsite::Domain> {
   private:
    IntraFn m_intraprocedural;
    Registry* m_registry;

   public:
    static CallerContext initial_domain() { return CallerContext(); }

    explicit CallGraphFixpointIterator(const CallGraph& graph,
                                       Registry* registry,
                                       const IntraFn& intraprocedural)
        : Analysis::template FixpointIteratorBase<CallGraphInterface,
                                                  CallerContext>(graph),
          m_intraprocedural(intraprocedural),
          m_registry(registry) {}

    virtual void analyze_node(const typename CallGraphInterface::NodeId& node,
                              CallerContext* current_state) const override {
      m_intraprocedural(
          Analysis::function_by_node_id(node), this->m_registry, current_state)
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
    static_assert(std::is_base_of<AbstractRegistry, Registry>::value,
                  "Registry must inherit from sparta::AbstractRegistry");

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

    std::shared_ptr<CallGraphFixpointIterator> fp = nullptr;

    for (int iteration = 0; iteration < m_max_iteration; iteration++) {
      auto callgraph = Analysis::call_graph_of(m_program, &this->registry);

      // TODO: remove or abstract logging
      std::cerr << "Iteration " << iteration + 1 << std::endl;

      fp = std::make_shared<CallGraphFixpointIterator>(
          callgraph,
          &this->registry,
          [this](const Function& func,
                 Registry* reg,
                 CallerContext* context) -> std::shared_ptr<FunctionAnalyzer> {
            // intraprocedural part
            return this->run_on_function(func, reg, context);
          });

      // TODO: double check this, I think it actually makes sense to join
      // the caller context domains then use it as the initial domain for
      // the next iteration.
      fp->run(CallGraphFixpointIterator::initial_domain());

      if (this->registry.has_update()) {
        this->registry.materialize_update();
      } else {
        std::cerr << "Global fixpoint reached after " << iteration + 1
                  << " iterations." << std::endl;
        break;
      }
    }

    return fp;
  }

  virtual std::shared_ptr<FunctionAnalyzer> run_on_function(
      const Function& function, Registry* reg, CallerContext* context) {

    auto analyzer = std::make_shared<FunctionAnalyzer>(function, reg, context);

    analyzer->analyze();
    return analyzer;
  }

 private:
  Program m_program;
  int m_max_iteration;
};

} // namespace sparta
