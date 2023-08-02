/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractDomain.h>

namespace sparta {

/*
 * This class defines the interface to a chaotic fixpoint iterator. A detailed
 * exposition of chaotic fixpoint iteration and its use in Abstract
 * Interpretation can be found in the following paper:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation and application to
 *  logic programs. Journal of Logic Programming, 13(2—3):103—179, 1992.
 *
 * A chaotic fixpoint iterator takes a control-flow graph (CFG) and an abstract
 * domain as inputs. The notion of CFG used here is very broad and includes call
 * graphs, dependency graphs of systems of semantic equations, etc.
 *
 * The interface to the CFG is specified by a structure that should have the
 * following layout:
 *
 * class CFG {
 *  using Graph = ...;
 *  using NodeId = ...;
 *  using EdgeId = ...;
 *
 *  static const NodeId entry(const Graph& graph) { ... }
 *  static const NodeId source(const Graph& graph, const EdgeId& e) { ... }
 *  static const NodeId target(const Graph& graph, const EdgeId& e) { ... }
 *
 *  // Edges is an arbitrary type representing a collection of edges. The only
 *  // requirement is that it must define a standard iterator interface.
 *  static Edges predecessors(const Graph& graph, const NodeId& m) { ... }
 *  static Edges successors(const Graph& graph, const NodeId& m) { ... }
 * }
 *
 */
template <typename GraphInterface, typename Domain>
class FixpointIterator {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;

  virtual ~FixpointIterator() {
    static_assert(std::is_base_of<AbstractDomain<Domain>, Domain>::value,
                  "Domain does not inherit from AbstractDomain");

    // Check that GraphInterface has the necessary methods.
    // We specify it here instead of putting the static asserts in the
    // destructor of a CRTP-style base class because the destructor may not be
    // instantiated when we don't create any instances of the GraphInterface
    // class.
    // The graph is specified by its root node together with the successors,
    // predecessors, and edge source/target functions.
    static_assert(
        std::is_same<decltype(GraphInterface::entry(std::declval<Graph>())),
                     NodeId>::value,
        "No implementation of entry()");
    static_assert(
        !std::is_same<
            typename std::iterator_traits<typename std::remove_reference<
                decltype(GraphInterface::predecessors(
                    std::declval<Graph>(),
                    std::declval<NodeId>()))>::type::iterator>::value_type,
            void>::value,
        "No implementation of predecessors() that returns an iterable type");
    static_assert(
        !std::is_same<
            typename std::iterator_traits<typename std::remove_reference<
                decltype(GraphInterface::successors(
                    std::declval<Graph>(),
                    std::declval<NodeId>()))>::type::iterator>::value_type,
            void>::value,
        "No implementation of successors() that returns an iterable type");
    static_assert(
        std::is_same<decltype(GraphInterface::source(std::declval<Graph>(),
                                                     std::declval<EdgeId>())),
                     NodeId>::value,
        "No implementation of source()");
    static_assert(
        std::is_same<decltype(GraphInterface::target(std::declval<Graph>(),
                                                     std::declval<EdgeId>())),
                     NodeId>::value,
        "No implementation of target()");
  }

  /*
   * This method implements the semantic transformer for each node in the
   * control-flow graph. For better performance, the transformer operates by
   * modifying the current state via side effects (hence the pointer to an
   * abstract value). The method is invoked with an abstract value describing
   * the state of the program upon entering the node. When the method returns,
   * the abstract value 'current_state' should contain the state of the program
   * after the node has been processed. If a node represents a basic block, the
   * same abstract value can be used in sequence to analyze all instructions in
   * the block, thus avoiding costly copies between instructions.
   *
   * Node transformers are required to be monotonic.
   *
   */
  virtual void analyze_node(const NodeId& node,
                            Domain* current_state) const = 0;

  /*
   * Edges in the control-flow graph may be associated with different behaviors
   * that have distinct semantics (conditional branch, exception, etc.). This
   * method describes the effect of traversing an outgoing edge on the state of
   * the program, when the source node is exited and control is transferred over
   * to the target node.
   *
   * Edge transformers are required to be monotonic.
   *
   */
  virtual Domain analyze_edge(const EdgeId& edge,
                              const Domain& exit_state_at_source) const = 0;
};

} // namespace sparta
