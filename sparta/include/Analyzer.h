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

// To achieve a high level of metaprogramming, we use a little C++ trick to
// check for existence of a member function. This is implemented with SFINAE.
//
// More details:
// https://en.wikibooks.org/wiki/More_C%2B%2B_Idioms/Member_Detector
// https://stackoverflow.com/questions/257288/templated-check-for-the-existence-of-a-class-member-function

#define HAS_MEM_FUNC(func, name)                                \
  template <typename T, typename Sign>                          \
  struct name {                                                 \
    typedef char yes[1];                                        \
    typedef char no[2];                                         \
    template <typename U, U>                                    \
    struct type_check;                                          \
    template <typename _1>                                      \
    static yes& chk(type_check<Sign, &_1::func>*);              \
    template <typename>                                         \
    static no& chk(...);                                        \
    static bool const value = sizeof(chk<T>(0)) == sizeof(yes); \
  }

template <bool C, typename T = void>
struct enable_if {
  typedef T type;
};

template <typename T>
struct enable_if<false, T> {};

HAS_MEM_FUNC(analyze_edge, has_analyze_edge);

// The compiler is going to optionally enable either of the following
template <typename Callsite, typename Edge, typename Domain>
typename enable_if<
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
typename enable_if<
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

// struct Callsite {
//  type Domain
//  analyze_edge (optional)
// }

template <typename Analysis, typename Metadata = void>
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
        const typename CallGraphInterface::EdgeId& edge,
        const CallerContext& exit_state_at_source) const override {
      return optionally_analyze_edge_if_exist<
          Callsite,
          typename CallGraphInterface::EdgeId,
          CallerContext>(nullptr, edge, exit_state_at_source);
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

  InterproceduralAnalyzer(Program program,
                          int max_iteration,
                          Metadata* metadata = nullptr)
      : m_program(std::move(program)),
        m_max_iteration(max_iteration),
        m_metadata(metadata) {}

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

    auto analyzer =
        std::make_shared<FunctionAnalyzer>(function, reg, context, m_metadata);

    analyzer->analyze();
    return analyzer;
  }

 private:
  Program m_program;
  int m_max_iteration;
  Metadata* m_metadata;
};

} // namespace sparta
