/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/HashedSetAbstractDomain.h>
#include <sparta/MonotonicFixpointIterator.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/PatriciaTreeSet.h>

#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/functional/hash.hpp>

namespace liveness {

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

template <template <typename GraphInterface, typename Domain, typename NodeHash>
          class FixpointIteratorBase>
class FixpointEngine final
    : public FixpointIteratorBase<
          BackwardsFixpointIterationAdaptor<ProgramInterface>,
          LivenessDomain,
          std::hash<typename ProgramInterface::NodeId>> {
 private:
  using Base =
      FixpointIteratorBase<BackwardsFixpointIterationAdaptor<ProgramInterface>,
                           LivenessDomain,
                           std::hash<typename ProgramInterface::NodeId>>;
  using EdgeId = typename Base::EdgeId;

 public:
  explicit FixpointEngine(const Program& program)
      : Base(program), m_program(program) {}

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
    return Base::get_exit_state_at(node);
  }

  LivenessDomain get_live_out_vars_at(const uint32_t& node) {
    // Similarly, the set of live variables after executing a node is given by
    // the entry state at the node.
    return Base::get_entry_state_at(node);
  }

 private:
  const Program& m_program;
};

} // namespace liveness

template <typename FixpointEngine>
class MonotonicFixpointIteratorLivenessTest : public ::testing::Test {
 protected:
  MonotonicFixpointIteratorLivenessTest()
      : m_program1(1), m_program2(1), m_program3(1) {}

  void SetUp() override {
    build_program1();
    build_program2();
    build_program3();
  }

  liveness::Program m_program1;
  liveness::Program m_program2;
  liveness::Program m_program3;

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
    using namespace liveness;
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
    using namespace liveness;
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
    using namespace liveness;
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

using LivenessFixpoints = ::testing::Types<
    liveness::FixpointEngine<sparta::WTOMonotonicFixpointIterator>,
    liveness::FixpointEngine<sparta::MonotonicFixpointIterator>,
    liveness::FixpointEngine<sparta::ParallelMonotonicFixpointIterator>>;
TYPED_TEST_CASE(MonotonicFixpointIteratorLivenessTest, LivenessFixpoints);

TYPED_TEST(MonotonicFixpointIteratorLivenessTest, program1) {
  using namespace std::placeholders;
  using namespace liveness;
  TypeParam fp(this->m_program1);
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

TYPED_TEST(MonotonicFixpointIteratorLivenessTest, program2) {
  using namespace std::placeholders;
  using namespace liveness;
  TypeParam fp(this->m_program2);
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

TYPED_TEST(MonotonicFixpointIteratorLivenessTest, program3) {
  using namespace std::placeholders;
  using namespace liveness;
  TypeParam fp(this->m_program3);
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

namespace numerical {

using namespace sparta;

/*
 * In order to test the fixpoint iterator, we implement a numerical analysis on
 * a skeleton language.
 */

/*
 * A statement of our language is either:
 * - An assignment: `x = 0`
 * - An addition: `x = y + 1`
 */
struct Statement {
  Statement() = default;
  virtual ~Statement() = default;
};

struct Assignment : public Statement {
  Assignment(std::string* variable, unsigned value)
      : variable(variable), value(value) {}

  std::string* variable;
  unsigned value;
};

struct Addition : public Statement {
  Addition(std::string* result, std::string* left, unsigned right)
      : result(result), left(left), right(right) {}

  std::string* result;
  std::string* left;
  unsigned right;
};

class BasicBlock;

struct Edge final {
  Edge(BasicBlock* source, BasicBlock* target)
      : source(source), target(target) {}

  BasicBlock* source;
  BasicBlock* target;
};

class BasicBlock final {
 public:
  BasicBlock() = default;

  void add(std::unique_ptr<Statement> statement) {
    m_statements.push_back(std::move(statement));
  }

  void add_successor(BasicBlock* successor) {
    m_edges.push_back(std::make_unique<Edge>(this, successor));
    auto* edge = m_edges.back().get();
    m_successors.push_back(edge);
    successor->m_predecessors.push_back(edge);
  }

  const std::vector<std::unique_ptr<Statement>>& statements() const {
    return m_statements;
  }

 private:
  std::vector<std::unique_ptr<Statement>> m_statements;
  std::vector<std::unique_ptr<Edge>> m_edges;
  std::vector<Edge*> m_predecessors;
  std::vector<Edge*> m_successors;

  friend class ProgramInterface;
};

class Program final {
 public:
  Program() = default;

  BasicBlock* create_block() {
    m_basic_blocks.push_back(std::make_unique<BasicBlock>());
    return m_basic_blocks.back().get();
  }

  void set_entry(BasicBlock* entry) { m_entry = entry; }

  void set_exit(BasicBlock* exit) { m_exit = exit; }

 private:
  std::vector<std::unique_ptr<BasicBlock>> m_basic_blocks;
  BasicBlock* m_entry = nullptr;
  BasicBlock* m_exit = nullptr;

  friend class ProgramInterface;
};

class ProgramInterface {
 public:
  using Graph = Program;
  using NodeId = BasicBlock*;
  using EdgeId = Edge*;

  static NodeId entry(const Graph& graph) { return graph.m_entry; }
  static NodeId exit(const Graph& graph) { return graph.m_exit; }
  static std::vector<EdgeId> predecessors(const Graph&, const NodeId& node) {
    return node->m_predecessors;
  }
  static std::vector<EdgeId> successors(const Graph&, const NodeId& node) {
    return node->m_successors;
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e->source; }
  static NodeId target(const Graph&, const EdgeId& e) { return e->target; }
};

/* A powerset of integers with a widening to top. */
class IntegerSetAbstractDomain final
    : public AbstractDomain<IntegerSetAbstractDomain> {
 public:
  IntegerSetAbstractDomain() : m_top(true) {}

  explicit IntegerSetAbstractDomain(std::initializer_list<unsigned> values)
      : m_set(values), m_top(false) {}

  static IntegerSetAbstractDomain bottom() {
    return IntegerSetAbstractDomain(/* top */ false);
  }

  static IntegerSetAbstractDomain top() {
    return IntegerSetAbstractDomain(/* top */ true);
  }

  bool is_bottom() const { return !m_top && m_set.empty(); }

  bool is_top() const { return m_top; }

  void set_to_bottom() {
    m_set.clear();
    m_top = false;
  }

  void set_to_top() {
    m_set.clear();
    m_top = true;
  }

  bool leq(const IntegerSetAbstractDomain& other) const {
    if (is_bottom() || other.is_top()) {
      return true;
    } else if (is_top() || other.is_bottom()) {
      return false;
    } else {
      return m_set.is_subset_of(other.m_set);
    }
  }

  bool equals(const IntegerSetAbstractDomain& other) const {
    if (is_bottom()) {
      return other.is_bottom();
    } else if (is_top()) {
      return other.is_top();
    } else {
      return m_set.equals(other.m_set);
    }
  }

  void join_with(const IntegerSetAbstractDomain& other) {
    if (is_top() || other.is_bottom()) {
      return;
    } else if (is_bottom() || other.is_top()) {
      *this = other;
    } else {
      m_set.union_with(other.m_set);
    }
  }

  void widen_with(const IntegerSetAbstractDomain& other) {
    if (is_top() || other.is_bottom()) {
      return;
    } else if (is_bottom() || other.is_top()) {
      *this = other;
    } else if (other.m_set.is_subset_of(m_set)) {
      return;
    } else {
      set_to_top();
    }
  }

  void meet_with(const IntegerSetAbstractDomain& other) {
    // Never used.
  }

  void narrow_with(const IntegerSetAbstractDomain& other) {
    // Never used.
  }

  /* Insert a value in the set. */
  void insert(unsigned value) {
    if (m_top) {
      return;
    }

    m_set.insert(value);
  }

  /* Add two integer sets. */
  static IntegerSetAbstractDomain add(const IntegerSetAbstractDomain& lhs,
                                      const IntegerSetAbstractDomain& rhs) {
    if (lhs.is_bottom() || rhs.is_bottom()) {
      return bottom();
    } else if (lhs.is_top() || rhs.is_top()) {
      return top();
    } else {
      auto result = IntegerSetAbstractDomain::bottom();
      for (unsigned x : lhs.m_set) {
        for (unsigned y : rhs.m_set) {
          result.insert(x + y);
        }
      }
      return result;
    }
  }

  friend std::ostream& operator<<(std::ostream& o,
                                  const IntegerSetAbstractDomain& set) {
    if (set.is_top()) {
      o << "T";
    } else if (set.is_bottom()) {
      o << "_|_";
    } else {
      o << set.m_set;
    }
    return o;
  }

 private:
  explicit IntegerSetAbstractDomain(bool top) : m_top(top) {}

  PatriciaTreeSet<unsigned> m_set;
  bool m_top;
};

using AbstractEnvironmentT =
    PatriciaTreeMapAbstractEnvironment<std::string*, IntegerSetAbstractDomain>;

template <template <typename GraphInterface, typename Domain, typename NodeHash>
          class FixpointIteratorBase>
class FixpointEngine final : public FixpointIteratorBase<
                                 ProgramInterface,
                                 AbstractEnvironmentT,
                                 std::hash<typename ProgramInterface::NodeId>> {
 private:
  using Base =
      FixpointIteratorBase<ProgramInterface,
                           AbstractEnvironmentT,
                           std::hash<typename ProgramInterface::NodeId>>;
  using NodeId = typename Base::NodeId;
  using EdgeId = typename Base::EdgeId;

 public:
  explicit FixpointEngine(const Program& program) : Base(program) {}

  void analyze_node(const NodeId& bb,
                    AbstractEnvironmentT* current_state) const override {
    for (const auto& statement : bb->statements()) {
      analyze_statement(statement.get(), current_state);
    }
  }

  void analyze_statement(Statement* statement,
                         AbstractEnvironmentT* current_state) const {
    if (auto* assign = dynamic_cast<Assignment*>(statement)) {
      current_state->set(assign->variable,
                         IntegerSetAbstractDomain{assign->value});
    } else if (auto* addition = dynamic_cast<Addition*>(statement)) {
      current_state->set(addition->result,
                         IntegerSetAbstractDomain::add(
                             current_state->get(addition->left),
                             IntegerSetAbstractDomain{addition->right}));
    } else {
      throw std::runtime_error("unreachable");
    }
  }

  AbstractEnvironmentT analyze_edge(
      const EdgeId&, const AbstractEnvironmentT& state) const override {
    return state;
  }
};

} // namespace numerical

template <typename FixpointEngine>
class MonotonicFixpointIteratorNumericalTest : public ::testing::Test {};

using NumericalFixpoints = ::testing::Types<
    numerical::FixpointEngine<sparta::WTOMonotonicFixpointIterator>,
    numerical::FixpointEngine<sparta::MonotonicFixpointIterator>,
    numerical::FixpointEngine<sparta::ParallelMonotonicFixpointIterator>>;
TYPED_TEST_CASE(MonotonicFixpointIteratorNumericalTest, NumericalFixpoints);

TYPED_TEST(MonotonicFixpointIteratorNumericalTest, program1) {
  using namespace numerical;

  /*
   * bb1: x = 1;
   *      if (...) {
   * bb2:   y = x + 1;
   *      } else {
   * bb3:   y = x + 2;
   *      }
   * bb4: return
   */
  Program program;

  BasicBlock* bb1 = program.create_block();
  BasicBlock* bb2 = program.create_block();
  BasicBlock* bb3 = program.create_block();
  BasicBlock* bb4 = program.create_block();

  std::string x = "x";
  std::string y = "y";

  bb1->add(std::make_unique<Assignment>(&x, 1));
  bb1->add_successor(bb2);
  bb1->add_successor(bb3);

  bb2->add(std::make_unique<Addition>(&y, &x, 1));
  bb2->add_successor(bb4);

  bb3->add(std::make_unique<Addition>(&y, &x, 2));
  bb3->add_successor(bb4);

  program.set_entry(bb1);
  program.set_exit(bb4);

  TypeParam fp(program);
  fp.run(AbstractEnvironmentT::top());

  EXPECT_EQ(fp.get_entry_state_at(bb1), AbstractEnvironmentT::top());
  EXPECT_EQ(fp.get_exit_state_at(bb1).get(&x), IntegerSetAbstractDomain{1});
  EXPECT_EQ(fp.get_exit_state_at(bb1).get(&y), IntegerSetAbstractDomain::top());

  EXPECT_EQ(fp.get_entry_state_at(bb2), fp.get_exit_state_at(bb1));
  EXPECT_EQ(fp.get_exit_state_at(bb2).get(&x), IntegerSetAbstractDomain{1});
  EXPECT_EQ(fp.get_exit_state_at(bb2).get(&y), IntegerSetAbstractDomain{2});

  EXPECT_EQ(fp.get_entry_state_at(bb3), fp.get_exit_state_at(bb1));
  EXPECT_EQ(fp.get_exit_state_at(bb3).get(&x), IntegerSetAbstractDomain{1});
  EXPECT_EQ(fp.get_exit_state_at(bb3).get(&y), IntegerSetAbstractDomain{3});

  EXPECT_EQ(fp.get_entry_state_at(bb4).get(&x), IntegerSetAbstractDomain{1});
  EXPECT_EQ(fp.get_entry_state_at(bb4).get(&y),
            (IntegerSetAbstractDomain{2, 3}));
  EXPECT_EQ(fp.get_exit_state_at(bb4), fp.get_entry_state_at(bb4));
}

TYPED_TEST(MonotonicFixpointIteratorNumericalTest, program2) {
  using namespace numerical;

  /*
   * bb1: x = 1;
   *      while (...) {
   * bb2:   x = x + 1;
   *      }
   * bb3: return
   */
  Program program;

  BasicBlock* bb1 = program.create_block();
  BasicBlock* bb2 = program.create_block();
  BasicBlock* bb3 = program.create_block();

  std::string x = "x";

  bb1->add(std::make_unique<Assignment>(&x, 1));
  bb1->add_successor(bb2);

  bb2->add(std::make_unique<Addition>(&x, &x, 1));
  bb2->add_successor(bb2);
  bb2->add_successor(bb3);

  program.set_entry(bb1);
  program.set_exit(bb3);

  TypeParam fp(program);
  fp.run(AbstractEnvironmentT::top());

  EXPECT_EQ(fp.get_entry_state_at(bb1), AbstractEnvironmentT::top());
  EXPECT_EQ(fp.get_exit_state_at(bb1).get(&x), IntegerSetAbstractDomain{1});

  EXPECT_EQ(fp.get_entry_state_at(bb2).get(&x),
            IntegerSetAbstractDomain::top());
  EXPECT_EQ(fp.get_exit_state_at(bb2).get(&x), IntegerSetAbstractDomain::top());

  EXPECT_EQ(fp.get_entry_state_at(bb3).get(&x),
            IntegerSetAbstractDomain::top());
  EXPECT_EQ(fp.get_exit_state_at(bb3).get(&x), IntegerSetAbstractDomain::top());
}
