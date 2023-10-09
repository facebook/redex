/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sparta/AbstractDomain.h>
#include <sparta/TypeTraits.h>

namespace sparta {

namespace {
SPARTA_HAS_MEMBER_FUNCTION_WITH_SIGNATURE(analyze_edge, has_analyze_edge);
}

// The compiler is going to optionally enable either of the following
template <typename Callsite, typename Edge, typename Domain>
typename std::enable_if<
    has_analyze_edge<Callsite,
                     Domain (Callsite::*)(const Edge&, const Domain&)>::value,
    Domain>::type
optionally_analyze_edge_if_exist(Callsite*,
                                 const Edge& edge,
                                 const Domain& domain) {
  static Callsite c2;
  return c2.analyze_edge(edge, domain);
}

template <typename Callsite, typename Edge, typename Domain>
typename std::enable_if<
    !has_analyze_edge<Callsite,
                      Domain (Callsite::*)(const Edge&, const Domain&)>::value,
    Domain>::type
optionally_analyze_edge_if_exist(Callsite* c,
                                 const Edge&,
                                 const Domain& domain) {
  return domain;
}

// Function level analyzer need to extend this class. This class supports the
// static assertion in the InterproceduralAnalyzer. This choice is made so that
// the compiler will throw reasonable errors when the provided function
// analyzers don't implement the necessary methods. We prefer this over template
// errors.

template <typename FunctionSummaries,
          typename CallerContext,
          typename CallGraph,
          typename AnalysisParameters = void>
class Intraprocedural {
 public:
  virtual void analyze() = 0;
  virtual void summarize() = 0;
  virtual ~Intraprocedural() {}
  void set_summaries(FunctionSummaries* summaries) {
    this->m_summaries = summaries;
  }
  void set_caller_context(CallerContext* context) {
    this->m_caller_context = context;
  }
  void set_call_graph(const CallGraph* graph) { this->m_call_graph = graph; }
  void set_analysis_parameters(AnalysisParameters* parameters) {
    this->m_analysis_parameters = parameters;
  }

 protected:
  FunctionSummaries* get_summaries() const { return this->m_summaries; }
  CallerContext* get_caller_context() const { return this->m_caller_context; }
  const CallGraph* get_call_graph() const { return this->m_call_graph; }
  AnalysisParameters* get_analysis_parameters() const {
    return this->m_analysis_parameters;
  }

 private:
  FunctionSummaries* m_summaries;
  CallerContext* m_caller_context;
  const CallGraph* m_call_graph;
  AnalysisParameters* m_analysis_parameters;
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

// struct Callsite {
//  type Domain
//  analyze_edge (optional)
// }

template <typename Analysis, typename AnalysisParameters = void>
class InterproceduralAnalyzer {
 public:
  using Function = typename Analysis::Function;
  using Program = typename Analysis::Program;
  using Registry = typename Analysis::Registry;

  using CallGraphInterface = typename Analysis::CallGraphInterface;

  using CallGraph = typename CallGraphInterface::Graph;
  using Callsite = typename Analysis::Callsite;
  using CallerContext = typename Callsite::Domain;

  using FunctionAnalyzer = typename Analysis::template FunctionAnalyzer<
      Intraprocedural<Registry, CallerContext, CallGraph, AnalysisParameters>>;

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
      m_intraprocedural(Analysis::function_by_node_id(node), this->m_registry,
                        current_state)
          ->summarize();
    }

    CallerContext analyze_edge(
        const typename CallGraphInterface::EdgeId& edge,
        const CallerContext& exit_state_at_source) const override {
      return optionally_analyze_edge_if_exist<
          Callsite, typename CallGraphInterface::EdgeId, CallerContext>(
          nullptr, edge, exit_state_at_source);
    }
  };

  virtual ~InterproceduralAnalyzer() {
    static_assert(std::is_base_of<AbstractRegistry, Registry>::value,
                  "Registry must inherit from sparta::AbstractRegistry");

    static_assert(
        std::is_base_of<Intraprocedural<Registry, CallerContext, CallGraph,
                                        AnalysisParameters>,
                        FunctionAnalyzer>::value,
        "FunctionAnalyzer must inherit from sparta::Intraprocedural");

    static_assert(
        std::is_base_of<AbstractDomain<CallerContext>, CallerContext>::value,
        "Callsite::Domain must inherit from sparta::AbstractDomain");
  }

  InterproceduralAnalyzer(Program program,
                          int max_iteration,
                          AnalysisParameters* parameters = nullptr)
      : m_program(std::move(program)),
        m_max_iteration(max_iteration),
        m_parameters(parameters) {}

  virtual std::shared_ptr<CallGraphFixpointIterator> run(
      bool rebuild_callgraph_on_each_iteration = false) {
    // keep a copy of old function summaries, do fixpoint on this level.

    std::shared_ptr<CallGraphFixpointIterator> fp = nullptr;
    boost::optional<CallGraph> callgraph = boost::none;

    for (int iteration = 0; iteration < m_max_iteration; iteration++) {
      if (m_logger) {
        (*m_logger)(std::string("Iteration ") + std::to_string(iteration + 1));
      }
      if (!callgraph || rebuild_callgraph_on_each_iteration) {
        callgraph = Analysis::call_graph_of(m_program, &this->registry);
      }

      if (!fp || rebuild_callgraph_on_each_iteration) {
        // If the callgraph requires to be rebuilt, we need to rebuild the
        // iterator as well as weak partial ordering should be updated.
        fp = std::make_shared<CallGraphFixpointIterator>(
            *callgraph,
            &this->registry,
            [this, &callgraph](
                const Function& func, Registry* reg,
                CallerContext* context) -> std::shared_ptr<FunctionAnalyzer> {
              // intraprocedural part
              return this->run_on_function(func, reg, context, &*callgraph);
            });
      }

      fp->run(CallGraphFixpointIterator::initial_domain());

      if (this->registry.has_update()) {
        this->registry.materialize_update();
      } else {
        if (m_logger) {
          (*m_logger)(std::string("Global fixpoint reached after ") +
                      std::to_string(iteration + 1) + " iterations.");
        }
        break;
      }
    }

    return fp;
  }

  virtual std::shared_ptr<FunctionAnalyzer> run_on_function(
      const Function& function,
      Registry* reg,
      CallerContext* context,
      const CallGraph* graph) {

    auto analyzer = std::make_shared<FunctionAnalyzer>(function);
    analyzer->set_summaries(reg);
    analyzer->set_caller_context(context);
    analyzer->set_call_graph(graph);
    analyzer->set_analysis_parameters(m_parameters);
    analyzer->analyze();
    return analyzer;
  }

  void set_logger(const std::function<void(const std::string&)>& logger) {
    m_logger = logger;
  }

 private:
  Program m_program;
  int m_max_iteration;
  AnalysisParameters* m_parameters;
  boost::optional<std::function<void(const std::string&)>> m_logger =
      boost::none;
};

} // namespace sparta
