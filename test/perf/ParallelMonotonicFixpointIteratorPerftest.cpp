/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MonotonicFixpointIterator.h"

#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HashedSetAbstractDomain.h"
#include "Thread.h"
#include "WorkQueue.h"

using namespace sparta;

/*
 * In order to test the fixpoint iterator, we implement a liveness analysis on a
 * skeleton language. A statement simply contains the variables it defines and
 * the variables it uses, which is all we need to perform liveness analysis.
 */
struct Statement {
  Statement() = default;

  Statement(std::initializer_list<uint32_t> use,
            std::initializer_list<uint32_t> def)
      : use(use), def(def) {}

  std::vector<uint32_t> use;
  std::vector<uint32_t> def;
};

/*
 * A program is a control-flow graph where each node is labeled with a
 * statement.
 */
class Program final {
 public:
  using Edge = std::pair<uint32_t, uint32_t>;
  using EdgeId = std::shared_ptr<Edge>;

  explicit Program(uint32_t entry) : m_entry(entry), m_exit(entry) {}

  std::vector<EdgeId> successors(uint32_t node) const {
    auto& succs = m_successors.at(node);
    return std::vector<EdgeId>(succs.begin(), succs.end());
  }

  std::vector<EdgeId> predecessors(uint32_t node) const {
    auto& preds = m_predecessors.at(node);
    return std::vector<EdgeId>(preds.begin(), preds.end());
  }

  const Statement& statement_at(uint32_t node) const {
    auto it = m_statements.find(node);
    if (it == m_statements.end()) {
      fail(node);
    }
    return it->second;
  }

  void add(uint32_t node, const Statement& stmt) {
    m_statements[node] = stmt;
    // Ensure that the pred/succ entries for the node are initialized
    m_predecessors[node];
    m_successors[node];
  }

  void add_edge(uint32_t src, uint32_t dst) {
    auto edge = std::make_shared<Edge>(src, dst);
    m_successors[src].insert(edge);
    m_predecessors[dst].insert(edge);
  }

  void set_exit(uint32_t exit) { m_exit = exit; }

 private:
  // In gtest, FAIL (or any ASSERT_* statement) can only be called from within a
  // function that returns void.
  void fail(uint32_t node) const { FAIL() << "No statement at node " << node; }

  uint32_t m_entry;
  uint32_t m_exit;
  std::unordered_map<uint32_t, Statement> m_statements;
  std::unordered_map<uint32_t, std::unordered_set<EdgeId>> m_successors;
  std::unordered_map<uint32_t, std::unordered_set<EdgeId>> m_predecessors;

  friend class ProgramInterface;
};

class ProgramInterface {
 public:
  using Graph = Program;
  using NodeId = uint32_t;
  using EdgeId = Program::EdgeId;

  static NodeId entry(const Graph& graph) { return graph.m_entry; }
  static NodeId exit(const Graph& graph) { return graph.m_exit; }
  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    return graph.predecessors(node);
  }
  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    return graph.successors(node);
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e->first; }
  static NodeId target(const Graph&, const EdgeId& e) { return e->second; }
};

/*
 * The abstract domain for liveness is just the powerset domain of variables.
 */
using LivenessDomain = HashedSetAbstractDomain<uint32_t>;

class FixpointEngine final
    : public MonotonicFixpointIterator<
          BackwardsFixpointIterationAdaptor<ProgramInterface>,
          LivenessDomain> {
 public:
  explicit FixpointEngine(const Program& program)
      : MonotonicFixpointIterator(program), m_program(program) {}

  void analyze_node(const uint32_t& node,
                    LivenessDomain* current_state) const override {
    const Statement& stmt = m_program.statement_at(node);
    // Let each thread sleep for 1 milliseconds when performing
    // analyze_node to make the cost of analysis bigger than
    // other overheads.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // This is the standard semantic definition of liveness.
    current_state->remove(stmt.def.begin(), stmt.def.end());
    current_state->add(stmt.use.begin(), stmt.use.end());
  }

  LivenessDomain analyze_edge(
      const EdgeId&,
      const LivenessDomain& exit_state_at_source) const override {
    // Edges have no semantic transformers attached.
    return exit_state_at_source;
  }

 private:
  const Program& m_program;
};

class ParallelFixpointEngine final
    : public ParallelMonotonicFixpointIterator<
          BackwardsFixpointIterationAdaptor<ProgramInterface>,
          LivenessDomain> {
 public:
  explicit ParallelFixpointEngine(const Program& program, uint32_t num_core)
      : ParallelMonotonicFixpointIterator(program, num_core),
        m_program(program) {}

  void analyze_node(const uint32_t& node,
                    LivenessDomain* current_state) const override {
    const Statement& stmt = m_program.statement_at(node);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // This is the standard semantic definition of liveness.
    current_state->remove(stmt.def.begin(), stmt.def.end());
    current_state->add(stmt.use.begin(), stmt.use.end());
  }

  LivenessDomain analyze_edge(
      const EdgeId&,
      const LivenessDomain& exit_state_at_source) const override {
    // Edges have no semantic transformers attached.
    return exit_state_at_source;
  }

 private:
  const Program& m_program;
};

class MonotonicFixpointIteratorTest {
 public:
  MonotonicFixpointIteratorTest() : m_program1(1) {}

  void SetUp() { build_program1(); }

  Program m_program1;

 private:
  /*
   *  1: a = 0; Switch to 2-2000
   *     2: b = a + 2;
   *     ...
   *     2000: b = a + 2000;
   *  2001:   return b;
   */
  void build_program1() {
    m_program1.add(1, Statement(/* use: */ {}, /* def: */ {0}));
    for (uint32_t i = 2; i <= 2000; ++i) {
      m_program1.add(i, Statement(/* use: */ {0}, /* def: */ {i}));
    }
    m_program1.add(2001, Statement(/* use: */ {0}, /* def: */ {}));
    for (int i = 2; i <= 2000; ++i) {
      m_program1.add_edge(1, i);
    }
    for (int i = 2; i <= 2000; ++i) {
      m_program1.add_edge(i, 2001);
    }
    m_program1.set_exit(2001);
  }
};

double calculate_speedup(const MonotonicFixpointIteratorTest& test,
                         uint32_t num_core) {
  using namespace std::placeholders;
  ParallelFixpointEngine para_fp(test.m_program1, num_core);
  auto para_start = std::chrono::high_resolution_clock::now();
  para_fp.run(LivenessDomain());
  auto para_end = std::chrono::high_resolution_clock::now();

  double duration2 = std::chrono::duration_cast<std::chrono::microseconds>(
                         para_end - para_start)
                         .count();
  return duration2;
}

int main() {
  printf("Begin!\n");
  MonotonicFixpointIteratorTest test;
  test.SetUp();
  FixpointEngine fp(test.m_program1);
  auto single_start = std::chrono::high_resolution_clock::now();
  fp.run(LivenessDomain());
  auto single_end = std::chrono::high_resolution_clock::now();
  double duration1 = std::chrono::duration_cast<std::chrono::microseconds>(
                         single_end - single_start)
                         .count();
  for (uint32_t i = 1; i <= redex_parallel::default_num_threads(); ++i) {
    printf("%u %lf\n", i, duration1 / calculate_speedup(test, i));
  }
}
