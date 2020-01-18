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

using namespace sparta;

/*
 * In order to test the fixpoint iterator, we implement a liveness analysis on a
 * skeleton language. A statement simply contains the variables it defines and
 * the variables it uses, which is all we need to perform liveness analysis.
 */
struct Statement {
  Statement() = default;

  Statement(std::initializer_list<std::string> use,
            std::initializer_list<std::string> def)
      : use(use), def(def) {}

  std::vector<std::string> use;
  std::vector<std::string> def;
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
using LivenessDomain = HashedSetAbstractDomain<std::string>;

class FixpointEngine final
    : public ParallelMonotonicFixpointIterator<
          BackwardsFixpointIterationAdaptor<ProgramInterface>,
          LivenessDomain> {
 public:
  explicit FixpointEngine(const Program& program)
      : ParallelMonotonicFixpointIterator(program), m_program(program) {}

  void analyze_node(const uint32_t& node,
                    LivenessDomain* current_state) const override {
    const Statement& stmt = m_program.statement_at(node);
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

  LivenessDomain get_live_in_vars_at(const uint32_t& node) {
    // Since we performed a backward analysis by reversing the control-flow
    // graph, the set of live variables before executing a node is given by
    // the exit state at the node.
    return get_exit_state_at(node);
  }

  LivenessDomain get_live_out_vars_at(const uint32_t& node) {
    // Similarly, the set of live variables after executing a node is given by
    // the entry state at the node.
    return get_entry_state_at(node);
  }

 private:
  const Program& m_program;
};

class ParallelFixpointIteratorTest : public ::testing::Test {
 protected:
  ParallelFixpointIteratorTest()
      : m_program1(1), m_program2(1), m_program3(1) {}

  virtual void SetUp() {
    build_program1();
    build_program2();
    build_program3();
  }

  Program m_program1;
  Program m_program2;
  Program m_program3;

 private:
  /*
   *                       live in          live out
   *  1: a = 0;             {c}              {a, c}
   *  2: b = a + 1;         {a, c}           {b, c}
   *  3: c = c + b;         {b, c}           {b, c}
   *  4: a = b * 2;         {b, c}           {a, c}
   *  5: if (a < 9) {       {a, c}           {a, c}
   *       goto 2;
   *     } else {
   *  6:   return c;        {c}              {}
   *     }
   */
  void build_program1() {
    m_program1.add(1, Statement(/* use: */ {}, /* def: */ {"a"}));
    m_program1.add(2, Statement(/* use: */ {"a"}, /* def: */ {"b"}));
    m_program1.add(3, Statement(/* use: */ {"c", "b"}, /* def: */ {"c"}));
    m_program1.add(4, Statement(/* use: */ {"b"}, /* def: */ {"a"}));
    m_program1.add(5, Statement(/* use: */ {"a"}, /* def: */ {}));
    m_program1.add(6, Statement(/* use: */ {"c"}, /* def: */ {}));
    m_program1.add_edge(1, 2);
    m_program1.add_edge(2, 3);
    m_program1.add_edge(3, 4);
    m_program1.add_edge(4, 5);
    m_program1.add_edge(5, 6);
    m_program1.add_edge(5, 2);
    m_program1.set_exit(6);
  }

  /*
   *                       live in          live out
   *  1: x = a + b;        {a, b}           {x, a, b}
   *  2: y = a * b;        {x, a, b}        {x, y, a, b}
   *  3: if (y > a) {      {x, y, a, b}     {x, y, a, b}
   *  4:   return x;       {x}              {}
   *     }
   *  5: a = a + 1;        {y, a, b}        {y, a, b}
   *  6: x = a + b;        {y, a, b}        {x, y, a, b}
   *     if (...) {
   *       goto 7;
   *     }
   *     goto 3;
   *  7: x = y + a;
   *
   */
  void build_program2() {
    m_program2.add(1, Statement(/* use: */ {"a", "b"}, /* def: */ {"x"}));
    m_program2.add(2, Statement(/* use: */ {"a", "b"}, /* def: */ {"y"}));
    m_program2.add(3, Statement(/* use: */ {"y", "a"}, /* def: */ {}));
    m_program2.add(4, Statement(/* use: */ {"x"}, /* def: */ {}));
    m_program2.add(5, Statement(/* use: */ {"a"}, /* def: */ {"a"}));
    m_program2.add(6, Statement(/* use: */ {"a", "b"}, /* def: */ {"x"}));
    m_program2.add(7, Statement(/* use: */ {"y", "a"}, /* def: */ {"x"}));
    m_program2.add_edge(1, 2);
    m_program2.add_edge(2, 3);
    m_program2.add_edge(3, 4);
    m_program2.add_edge(3, 5);
    m_program2.add_edge(5, 6);
    m_program2.add_edge(6, 3);
    m_program2.add_edge(6, 7);
    m_program2.set_exit(4);
  }

  /*
   *                          live in           live out
   *  1: a, b -> x, y         {a, b, z}         {a, b, x, y, z}
   *  2: x, y -> z            {x, y, a, b}      {a, b, y, z}
   *  3: a -> c               {a, b, y, z}      {c, b, y, z}
   *  4: b -> d               {c, b, y, z}      {c, d, y, z}
   *  5: c, d -> a, b         {c, d, y, z}      {a, b, y, z}
   *  6: a, b -> x            {a, b, y, z}      {a, b, x, y, z}
   *  7: return z             {z}               {}
   *  8: a, b -> c, d         {a, b, y, z}      {c, b, y, z}
   *
   *  1->2, 2->3, 3->4, 4->5, 5->6, 6->7, 6->2, 5->3, 1->8, 8->4
   *  A test using graph that can reproduce error fixed in
   *  https://github.com/facebookincubator/SPARTA/pull/7
   */
  void build_program3() {
    m_program3.add(1, Statement(/* use: */ {"a", "b"}, /* def: */ {"x", "y"}));
    m_program3.add(2, Statement(/* use: */ {"x", "y"}, /* def: */ {"z"}));
    m_program3.add(3, Statement(/* use: */ {"a"}, /* def: */ {"c"}));
    m_program3.add(4, Statement(/* use: */ {"b"}, /* def: */ {"d"}));
    m_program3.add(5, Statement(/* use: */ {"c", "d"}, /* def: */ {"a", "b"}));
    m_program3.add(6, Statement(/* use: */ {"a", "b"}, /* def: */ {"x"}));
    m_program3.add(7, Statement(/* use: */ {"z"}, /* def: */ {}));
    m_program3.add(8, Statement(/* use: */ {"a", "b"}, /* def: */ {"c", "d"}));
    m_program3.add_edge(1, 2);
    m_program3.add_edge(2, 3);
    m_program3.add_edge(3, 4);
    m_program3.add_edge(4, 5);
    m_program3.add_edge(5, 6);
    m_program3.add_edge(6, 7);
    m_program3.add_edge(6, 2);
    m_program3.add_edge(5, 3);
    m_program3.add_edge(1, 8);
    m_program3.add_edge(8, 4);
    m_program3.set_exit(7);
  }
};

TEST_F(ParallelFixpointIteratorTest, program1) {
  using namespace std::placeholders;
  FixpointEngine fp(this->m_program1);
  fp.run(LivenessDomain());

  ASSERT_TRUE(fp.get_live_in_vars_at(1).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(1).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("c"));
  EXPECT_THAT(fp.get_live_out_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at(2).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(2).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("a", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("b", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at(3).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(3).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("b", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("b", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at(4).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(4).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(4).elements(),
              ::testing::UnorderedElementsAre("b", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at(4).elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at(5).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(5).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("a", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at(6).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(6).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(6).elements(),
              ::testing::UnorderedElementsAre("c"));
  EXPECT_TRUE(fp.get_live_out_vars_at(6).elements().empty());
}

TEST_F(ParallelFixpointIteratorTest, program2) {
  using namespace std::placeholders;
  FixpointEngine fp(this->m_program2);
  fp.run(LivenessDomain());

  ASSERT_TRUE(fp.get_live_in_vars_at(1).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(1).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("x", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(2).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(2).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("x", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(3).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(3).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(4).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(4).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(4).elements(),
              ::testing::UnorderedElementsAre("x"));
  EXPECT_TRUE(fp.get_live_out_vars_at(4).elements().empty());

  ASSERT_TRUE(fp.get_live_in_vars_at(5).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(5).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(6).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(6).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(6).elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(6).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(7).is_bottom());
  ASSERT_TRUE(fp.get_live_out_vars_at(7).is_bottom());
}

TEST_F(ParallelFixpointIteratorTest, program3) {
  using namespace std::placeholders;
  FixpointEngine fp(this->m_program3);
  fp.run(LivenessDomain());

  ASSERT_TRUE(fp.get_live_in_vars_at(1).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(1).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("a", "b", "z"));
  EXPECT_THAT(fp.get_live_out_vars_at(1).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b", "z"));

  ASSERT_TRUE(fp.get_live_in_vars_at(2).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(2).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(2).elements(),
              ::testing::UnorderedElementsAre("z", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(3).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(3).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("z", "y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(3).elements(),
              ::testing::UnorderedElementsAre("z", "y", "c", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at(4).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(4).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(4).elements(),
              ::testing::UnorderedElementsAre("z", "y", "c", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at(4).elements(),
              ::testing::UnorderedElementsAre("z", "y", "c", "d"));

  ASSERT_TRUE(fp.get_live_in_vars_at(5).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(5).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("z", "y", "c", "d"));
  EXPECT_THAT(fp.get_live_out_vars_at(5).elements(),
              ::testing::UnorderedElementsAre("z", "a", "b", "y"));

  ASSERT_TRUE(fp.get_live_in_vars_at(6).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(6).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(6).elements(),
              ::testing::UnorderedElementsAre("z", "a", "b", "y"));
  EXPECT_THAT(fp.get_live_out_vars_at(6).elements(),
              ::testing::UnorderedElementsAre("z", "a", "b", "x", "y"));

  ASSERT_TRUE(fp.get_live_in_vars_at(7).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(7).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(7).elements(),
              ::testing::UnorderedElementsAre("z"));
  EXPECT_TRUE(fp.get_live_out_vars_at(7).elements().empty());

  ASSERT_TRUE(fp.get_live_in_vars_at(8).is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at(8).is_value());
  EXPECT_THAT(fp.get_live_in_vars_at(8).elements(),
              ::testing::UnorderedElementsAre("z", "a", "b", "y"));
  EXPECT_THAT(fp.get_live_out_vars_at(8).elements(),
              ::testing::UnorderedElementsAre("z", "c", "b", "y"));
}
