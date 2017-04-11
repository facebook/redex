/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "FixpointIterators.h"
#include "HashedSetAbstractDomain.h"

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
  Program(const std::string& entry) : m_entry(entry) {}

  std::vector<std::string> successors(const std::string& node) {
    auto& succs = m_successors[node];
    return std::vector<std::string>(succs.begin(), succs.end());
  }

  std::vector<std::string> predecessors(const std::string& node) {
    auto& preds = m_predecessors[node];
    return std::vector<std::string>(preds.begin(), preds.end());
  }

  const Statement& statement_at(const std::string& node) const {
    auto it = m_statements.find(node);
    if (it == m_statements.end()) {
      fail(node);
    }
    return it->second;
  }

  void add(const std::string& node, const Statement& stmt) {
    m_statements[node] = stmt;
  }

  void add_edge(const std::string& src, const std::string& dst) {
    m_successors[src].insert(dst);
    m_predecessors[dst].insert(src);
  }

 private:
  // In gtest, FAIL (or any ASSERT_* statement) can only be called from within a
  // function that returns void.
  void fail(std::string node) const {
    FAIL() << "No statement at node " << node;
  }

  std::string m_entry;
  std::unordered_map<std::string, Statement> m_statements;
  std::unordered_map<std::string, std::unordered_set<std::string>> m_successors;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      m_predecessors;
};

/*
 * The abstract domain for liveness is just the powerset domain of variables.
 */
using LivenessDomain = HashedSetAbstractDomain<std::string>;

class FixpointIterator final
    : public MonotonicFixpointIterator<std::string, LivenessDomain> {
 public:
  // Liveness is a backward analysis, hence we apply the generic fixpoint
  // iterator by using the exit node as the root and swapping the successors and
  // predecessors functions.
  FixpointIterator(
      const Program& program,
      const std::string& exit_node,
      std::function<std::vector<std::string>(const std::string&)> successors,
      std::function<std::vector<std::string>(const std::string&)> predecessors)
      : MonotonicFixpointIterator(exit_node, predecessors, successors),
        m_program(program) {}

  void analyze_node(const std::string& node,
                    LivenessDomain* current_state) const override {
    const Statement& stmt = m_program.statement_at(node);
    // This is the standard semantic definition of liveness.
    current_state->remove(stmt.def.begin(), stmt.def.end());
    current_state->add(stmt.use.begin(), stmt.use.end());
  }

  LivenessDomain analyze_edge(
      const std::string& source,
      const std::string& target,
      const LivenessDomain& exit_state_at_source) const override {
    // Edges have no semantic transformers attached.
    return exit_state_at_source;
  }

  LivenessDomain get_live_in_vars_at(const std::string& node) {
    // Since we performed a backward analysis by reversing the control-flow
    // graph, the set of live variables before executing a node is given by
    // the exit state at the node.
    return get_exit_state_at(node);
  }

  LivenessDomain get_live_out_vars_at(const std::string& node) {
    // Similarly, the set of live variables after executing a node is given by
    // the entry state at the node.
    return get_entry_state_at(node);
  }

 private:
  const Program& m_program;
};

class MonotonicFixpointIteratorTest : public ::testing::Test {
 protected:
  MonotonicFixpointIteratorTest() : m_program1("1"), m_program2("1") {}

  virtual void SetUp() {
    build_program1();
    build_program2();
  }

  Program m_program1;
  Program m_program2;

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
    m_program1.add("1", Statement(/* use: */ {}, /* def: */ {"a"}));
    m_program1.add("2", Statement(/* use: */ {"a"}, /* def: */ {"b"}));
    m_program1.add("3", Statement(/* use: */ {"c", "b"}, /* def: */ {"c"}));
    m_program1.add("4", Statement(/* use: */ {"b"}, /* def: */ {"a"}));
    m_program1.add("5", Statement(/* use: */ {"a"}, /* def: */ {}));
    m_program1.add("6", Statement(/* use: */ {"c"}, /* def: */ {}));
    m_program1.add_edge("1", "2");
    m_program1.add_edge("2", "3");
    m_program1.add_edge("3", "4");
    m_program1.add_edge("4", "5");
    m_program1.add_edge("5", "6");
    m_program1.add_edge("5", "2");
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
   *     goto 3;
   *
   */
  void build_program2() {
    m_program2.add("1", Statement(/* use: */ {"a", "b"}, /* def: */ {"x"}));
    m_program2.add("2", Statement(/* use: */ {"a", "b"}, /* def: */ {"y"}));
    m_program2.add("3", Statement(/* use: */ {"y", "a"}, /* def: */ {}));
    m_program2.add("4", Statement(/* use: */ {"x"}, /* def: */ {}));
    m_program2.add("5", Statement(/* use: */ {"a"}, /* def: */ {"a"}));
    m_program2.add("6", Statement(/* use: */ {"a", "b"}, /* def: */ {"x"}));
    m_program2.add_edge("1", "2");
    m_program2.add_edge("2", "3");
    m_program2.add_edge("3", "4");
    m_program2.add_edge("3", "5");
    m_program2.add_edge("5", "6");
    m_program2.add_edge("6", "3");
  }
};

TEST_F(MonotonicFixpointIteratorTest, program1) {
  using namespace std::placeholders;
  FixpointIterator fp(this->m_program1,
                      "6",
                      std::bind(&Program::successors, &m_program1, _1),
                      std::bind(&Program::predecessors, &m_program1, _1));
  fp.run(LivenessDomain());

  ASSERT_TRUE(fp.get_live_in_vars_at("1").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("1").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("1").elements(),
              ::testing::UnorderedElementsAre("c"));
  EXPECT_THAT(fp.get_live_out_vars_at("1").elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at("2").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("2").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("2").elements(),
              ::testing::UnorderedElementsAre("a", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at("2").elements(),
              ::testing::UnorderedElementsAre("b", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at("3").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("3").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("3").elements(),
              ::testing::UnorderedElementsAre("b", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at("3").elements(),
              ::testing::UnorderedElementsAre("b", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at("4").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("4").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("4").elements(),
              ::testing::UnorderedElementsAre("b", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at("4").elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at("5").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("5").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("5").elements(),
              ::testing::UnorderedElementsAre("a", "c"));
  EXPECT_THAT(fp.get_live_out_vars_at("5").elements(),
              ::testing::UnorderedElementsAre("a", "c"));

  ASSERT_TRUE(fp.get_live_in_vars_at("6").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("6").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("6").elements(),
              ::testing::UnorderedElementsAre("c"));
  EXPECT_TRUE(fp.get_live_out_vars_at("6").elements().empty());
}

TEST_F(MonotonicFixpointIteratorTest, program2) {
  using namespace std::placeholders;
  FixpointIterator fp(this->m_program2,
                      "4",
                      std::bind(&Program::successors, &m_program2, _1),
                      std::bind(&Program::predecessors, &m_program2, _1));
  fp.run(LivenessDomain());

  ASSERT_TRUE(fp.get_live_in_vars_at("1").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("1").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("1").elements(),
              ::testing::UnorderedElementsAre("a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at("1").elements(),
              ::testing::UnorderedElementsAre("x", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at("2").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("2").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("2").elements(),
              ::testing::UnorderedElementsAre("x", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at("2").elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at("3").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("3").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("3").elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at("3").elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at("4").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("4").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("4").elements(),
              ::testing::UnorderedElementsAre("x"));
  EXPECT_TRUE(fp.get_live_out_vars_at("4").elements().empty());

  ASSERT_TRUE(fp.get_live_in_vars_at("5").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("5").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("5").elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at("5").elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));

  ASSERT_TRUE(fp.get_live_in_vars_at("6").is_value());
  ASSERT_TRUE(fp.get_live_out_vars_at("6").is_value());
  EXPECT_THAT(fp.get_live_in_vars_at("6").elements(),
              ::testing::UnorderedElementsAre("y", "a", "b"));
  EXPECT_THAT(fp.get_live_out_vars_at("6").elements(),
              ::testing::UnorderedElementsAre("x", "y", "a", "b"));
}
