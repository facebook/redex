/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <functional>
#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GlobalConstProp.h"

using namespace std;
using namespace std::placeholders;

struct Statement {
 public:
  enum Type { NARROW_MOVE, WIDE_MOVE, NARROW_CONST, WIDE_CONST, OTHER };

  Statement()
      : dest(0xffff), wide_value(0xffffffffffffffffL), type(Type::OTHER) {}

  // Constant assignments
  // Register to 32bit constant assignment
  Statement(uint16_t d, int32_t v)
      : dest(d), narrow_value(v), type(Type::NARROW_CONST) {}

  // Register to 64bit constant assignment
  Statement(uint16_t d, int64_t value)
      : dest(d), wide_value(value), type(Type::WIDE_CONST) {}

  // Reg-Reg Assignment
  Statement(uint16_t d, uint16_t s, bool wide = false)
      : dest(d), source(s), type(wide ? Type::WIDE_MOVE : Type::NARROW_MOVE) {}

  Statement(uint16_t d, bool wide = false)
      : dest(d), is_wide(wide), type(Type::OTHER) {}

  uint16_t dest;
  union {
    uint16_t source;
    int32_t narrow_value;
    int64_t wide_value;
    bool is_wide;
  };
  Type type;
};

std::ostream& operator<<(std::ostream& o, const Statement& stmt) {
  const char* type = "";

  switch (stmt.type) {
  case Statement::Type::NARROW_MOVE:
    type = "Narrow Move";
    break;
  case Statement::Type::WIDE_MOVE:
    type = "Wide Move";
    break;
  case Statement::Type::NARROW_CONST:
    type = "Narrow Const";
    break;
  case Statement::Type::WIDE_CONST:
    type = "Wide Const";
    break;
  case Statement::Type::OTHER:
    type = "Other";
    break;
  }

  o << "[Statement Type: " << type << ", Dest-reg: " << stmt.dest << "]";
  return o;
}

struct SimpleBlock {
  int num;
  vector<Statement> stmts;

  int id() const { return num; }
};

std::ostream& operator<<(std::ostream& o, const SimpleBlock& block) {
  o << "[Block ID: " << block.id() << ", Statements: {";
  for (const auto& stmt : block.stmts) {
    o << stmt << ", ";
  }
  o << "}]";
  return o;
}

class StatementIterable {
 public:
  StatementIterable(SimpleBlock* block) : m_block(block) {}

  vector<Statement>::iterator begin() const { return m_block->stmts.begin(); }
  vector<Statement>::iterator end() const { return m_block->stmts.end(); }

 private:
  SimpleBlock* m_block;
};

struct Program {
  Program() : m_start(nullptr) {}

  vector<SimpleBlock*> succ(SimpleBlock* block) const {
    auto& succs = m_successors.at(block);
    return vector<SimpleBlock*>(succs.begin(), succs.end());
  }

  vector<SimpleBlock*> pred(SimpleBlock* block) const {
    auto& preds = m_predecessors.at(block);
    return vector<SimpleBlock*>(preds.begin(), preds.end());
  }

  void add(SimpleBlock* block) {
    m_blocks.push_back(block);
    block->num = m_blocks.size() - 1;
    // Ensure these are default-initialized
    m_successors[block];
    m_predecessors[block];
  }

  void add_edge(SimpleBlock* source, SimpleBlock* dest) {
    m_successors[source].insert(dest);
    m_predecessors[dest].insert(source);
  }

  void set_start_block(SimpleBlock* block) { m_start = block; }

  SimpleBlock* get(int id) {
    assert(id < m_blocks.size());
    return m_blocks[id];
  }

  vector<SimpleBlock*>::iterator begin() { return m_blocks.begin(); }
  vector<SimpleBlock*>::iterator end() { return m_blocks.end(); }

  SimpleBlock* start() const { return m_start; }

 private:
  SimpleBlock* m_start;
  vector<SimpleBlock*> m_blocks;
  unordered_map<SimpleBlock*, unordered_set<SimpleBlock*>> m_successors;
  unordered_map<SimpleBlock*, unordered_set<SimpleBlock*>> m_predecessors;

  friend std::ostream& operator<<(std::ostream&, const Program&);
  friend class BlockIterable;
};

class ProgramInterface : public FixpointIteratorGraphSpec<ProgramInterface> {
 public:
  using Graph = Program;
  using NodeId = SimpleBlock*;
  using EdgeId = SimpleBlock*;

  static NodeId entry(const Graph& graph) { return graph.start(); }
  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId node) {
    return graph.pred(node);
  }
  static std::vector<EdgeId> successors(const Graph& graph, const NodeId node) {
    return graph.succ(node);
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e; }
  static NodeId target(const Graph&, const EdgeId& e) { return e; }
};

std::ostream& operator<<(std::ostream& o, const Program& p) {
  for (const auto block : p.m_blocks) {
    o << *block << "\n";
  }
  return o;
}

class BlockIterable {
 public:
  explicit BlockIterable(Program* p) : m_program(p) {}

  vector<SimpleBlock*>::iterator begin() const {
    return m_program->m_blocks.begin();
  }
  vector<SimpleBlock*>::iterator end() const {
    return m_program->m_blocks.end();
  }

 private:
  Program* m_program;
};

class SkeletonConstantPropAnalysis final
    : public ConstantPropFixpointAnalysis<ProgramInterface,
                                          Statement,
                                          BlockIterable,
                                          StatementIterable> {
 public:
  explicit SkeletonConstantPropAnalysis(Program& p)
      : ConstantPropFixpointAnalysis(p, BlockIterable(&p)), m_program(p) {}

  void analyze_instruction(const Statement& stmt,
                           ConstPropEnvironment* current_state) const override {
    switch (stmt.type) {
    case Statement::NARROW_CONST: {
      ConstPropEnvUtil::set_narrow(
          *current_state, stmt.dest, stmt.narrow_value);
    } break;
    case Statement::WIDE_CONST: {
      ConstPropEnvUtil::set_wide(*current_state, stmt.dest, stmt.wide_value);
    } break;
    case Statement::NARROW_MOVE: {
      // Found a constant definition, propagate.
      if (ConstPropEnvUtil::is_narrow_constant(*current_state, stmt.source)) {
        auto value = ConstPropEnvUtil::get_narrow(*current_state, stmt.source);
        ConstPropEnvUtil::set_narrow(*current_state, stmt.dest, value);
      }
    } break;
    case Statement::WIDE_MOVE: {
      // Found a constant definition, propagate.
      if (ConstPropEnvUtil::is_wide_constant(*current_state, stmt.source)) {
        auto value = ConstPropEnvUtil::get_wide(*current_state, stmt.source);
        ConstPropEnvUtil::set_wide(*current_state, stmt.dest, value);
      }
    } break;
    case Statement::OTHER: {
      ConstPropEnvUtil::set_top(*current_state, stmt.dest, stmt.is_wide);
    } break;
    }
  }

  void simplify_instruction(
      SimpleBlock* const& block,
      Statement& stmt,
      const ConstPropEnvironment& current_state) const override {
    switch (stmt.type) {
    case Statement::Type::NARROW_MOVE: {
      if (ConstPropEnvUtil::is_narrow_constant(current_state, stmt.source)) {
        stmt.type = Statement::NARROW_CONST;
        stmt.narrow_value =
            ConstPropEnvUtil::get_narrow(current_state, stmt.source);
      }
      break;
    }
    case Statement::Type::WIDE_MOVE: {
      if (ConstPropEnvUtil::is_wide_constant(current_state, stmt.source)) {
        stmt.type = Statement::WIDE_CONST;
        stmt.wide_value =
            ConstPropEnvUtil::get_wide(current_state, stmt.source);
      }
      break;
    }
    default: {}
    }
  }

 private:
  Program& m_program;
};

class GlobalConstantPropagationTest : public ::testing::Test {
 protected:
  GlobalConstantPropagationTest() = default;

  virtual void SetUp() {
    build_program1();
    build_program2();
    build_program3();
    build_program4();
  }

  Program m_program1;
  Program m_program2;
  Program m_program3;
  Program m_program4;

 private:
  /*
    r0 = 2;                 |
    r1 = 0x1234567890ACDEF; | B0
    if (c > 3) {
      r0 = 4;               | B1
    } else {
      r3 = ...;             | B2
    }
    r3 = r1;                | B3
    r4 = r0;                |
  */
  void build_program1() {
    auto b0 = new SimpleBlock;
    b0->stmts.push_back(Statement(0, 2));
    b0->stmts.push_back(Statement(1, int64_t(0x1234567890ABCDEFL)));
    m_program1.add(b0);

    auto b1 = new SimpleBlock;
    b1->stmts.push_back(Statement(0, 4));
    m_program1.add(b1);

    auto b2 = new SimpleBlock;
    b2->stmts.push_back(Statement(3));
    m_program1.add(b2);

    auto b3 = new SimpleBlock;
    b3->stmts.push_back(Statement(3, uint16_t(1), true));
    b3->stmts.push_back(Statement(4, uint16_t(0)));
    m_program1.add(b3);

    m_program1.set_start_block(b0);

    m_program1.add_edge(b0, b1);
    m_program1.add_edge(b0, b2);
    m_program1.add_edge(b1, b3);
    m_program1.add_edge(b2, b3);
  }

  /*
    r0 = 0;             |
    r1 = r0;            | B0
    r2 = 10L;           |
    while (r4 < 13) {
      r1 = ...          | B1
      if (r2 == 5) {
        r2 = 10L;       | B2
      } else {
        r2 = 10L;       | B3
      }
      r1 = r0;          |
      r4 = r4 + 1;      | B4
    }
    r4 = r0;            | B5
    r5 = r2;            |
  */
  void build_program2() {
    auto b0 = new SimpleBlock;
    b0->stmts.push_back(Statement(0, 0));
    b0->stmts.push_back(Statement(1, uint16_t(0)));
    b0->stmts.push_back(Statement(2, int64_t(10L)));
    m_program2.add(b0);

    auto b1 = new SimpleBlock;
    b1->stmts.push_back(Statement(1));
    m_program2.add(b1);

    auto b2 = new SimpleBlock;
    b2->stmts.push_back(Statement(2, int64_t(10L)));
    m_program2.add(b2);

    auto b3 = new SimpleBlock;
    b3->stmts.push_back(Statement(2, int64_t(10L)));
    m_program2.add(b3);

    auto b4 = new SimpleBlock;
    b4->stmts.push_back(Statement(1, uint16_t(0)));
    b4->stmts.push_back(Statement(4, true));
    m_program2.add(b4);

    auto b5 = new SimpleBlock;
    b5->stmts.push_back(Statement(4, uint16_t(0)));
    b5->stmts.push_back(Statement(5, uint16_t(2), true));
    m_program2.add(b5);

    m_program2.set_start_block(b0);

    m_program2.add_edge(b0, b1);
    m_program2.add_edge(b0, b5);
    m_program2.add_edge(b1, b2);
    m_program2.add_edge(b1, b3);
    m_program2.add_edge(b2, b4);
    m_program2.add_edge(b3, b4);
    m_program2.add_edge(b4, b5);
    m_program2.add_edge(b4, b1);
  }

  /*
    r0 = 0;      |
    r1 = 1;      | B0
    r2 = 2;      |
    r3 = 3;      |
    for (...) {
      r1 = 2L;   |
      r0 = 1L;   | B1
      r2 = 5;    |
      if (..) {
        r1 = 3L; | B2
      } else {
        r1 = 2L; | B3
      }
      r1 = 2;    | B4
    }
    r6 = r2;     | B5
    if (...) {
      r6 = r3;   | B6
    } else {
      r6 = r3;   | B7
    }
    r7 = r6;     |
    r4 = r0;     | B8
    r3 = r2;     |
  */
  void build_program3() {
    auto b0 = new SimpleBlock;
    b0->stmts.push_back(Statement(0, 0));
    b0->stmts.push_back(Statement(1, 1));
    b0->stmts.push_back(Statement(2, 2));
    b0->stmts.push_back(Statement(3, 3));
    m_program3.add(b0);

    auto b1 = new SimpleBlock;
    b1->stmts.push_back(Statement(1, int64_t(2L)));
    b1->stmts.push_back(Statement(0, int64_t(1L)));
    b1->stmts.push_back(Statement(2, 5));
    m_program3.add(b1);

    auto b2 = new SimpleBlock;
    b2->stmts.push_back(Statement(1, int64_t(3L)));
    m_program3.add(b2);

    auto b3 = new SimpleBlock;
    b3->stmts.push_back(Statement(1, int64_t(2L)));
    m_program3.add(b3);

    auto b4 = new SimpleBlock;
    b4->stmts.push_back(Statement(1, 2));
    m_program3.add(b4);

    auto b5 = new SimpleBlock;
    b5->stmts.push_back(Statement(6, uint16_t(2)));
    m_program3.add(b5);

    auto b6 = new SimpleBlock;
    b6->stmts.push_back(Statement(6, uint16_t(3)));
    m_program3.add(b6);

    auto b7 = new SimpleBlock;
    b7->stmts.push_back(Statement(6, uint16_t(3)));
    m_program3.add(b7);

    auto b8 = new SimpleBlock;
    b8->stmts.push_back(Statement(7, uint16_t(6)));
    b8->stmts.push_back(Statement(4, uint16_t(0)));
    b8->stmts.push_back(Statement(3, uint16_t(2)));
    m_program3.add(b8);

    m_program3.set_start_block(b0);

    m_program3.add_edge(b0, b1);
    m_program3.add_edge(b0, b5);
    m_program3.add_edge(b1, b2);
    m_program3.add_edge(b1, b3);
    m_program3.add_edge(b2, b4);
    m_program3.add_edge(b3, b4);
    m_program3.add_edge(b4, b5);
    m_program3.add_edge(b4, b1);
    m_program3.add_edge(b5, b6);
    m_program3.add_edge(b5, b7);
    m_program3.add_edge(b6, b8);
    m_program3.add_edge(b7, b8);
  }
  /*
    r1 = 1;           | B0
    while (true) { <------------- r2 = 1; | B2
      r2 = 2;         |
      r3 = r1;        | B1
      r4 = r2;        |
    }
  */
  void build_program4() {
    auto b0 = new SimpleBlock;
    b0->stmts.push_back(Statement(1, int32_t(1)));
    m_program4.add(b0);

    auto b1 = new SimpleBlock;
    b1->stmts.push_back(Statement(2, int32_t(2)));
    b1->stmts.push_back(Statement(3, uint16_t(1)));
    b1->stmts.push_back(Statement(4, uint16_t(2)));
    m_program4.add(b1);

    auto b2 = new SimpleBlock;
    b2->stmts.push_back(Statement(2, int32_t(1)));
    m_program4.add(b2);

    m_program4.set_start_block(b0);

    m_program4.add_edge(b0, b1);
    m_program4.add_edge(b1, b1);
    m_program4.add_edge(b2, b1);
  }
};

void print_constants_in(Program& p, SkeletonConstantPropAnalysis& a) {
  for (const auto& block : p) {
    printf("Block ID: %d\n", block->id());
    const auto& constants_in = a.get_constants_at_entry(block);
    cout << constants_in << "\n";
  }
}

TEST_F(GlobalConstantPropagationTest, testProgram1) {
  using namespace testing;

  SkeletonConstantPropAnalysis constant_prop(m_program1);
  constant_prop.run(ConstPropEnvironment());

  // Check constant facts in each every block
  auto get_const_env = [&](int id) {
    return constant_prop.get_constants_at_entry(m_program1.get(id));
  };

  // Block 0 -> Top
  EXPECT_TRUE(get_const_env(0).is_top());

  // Block 1 -> [r0: 2, r1: 0x1234567890ABCDEFL]
  EXPECT_EQ(get_const_env(1).get(0).value(),
            ConstantValue(2, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(
      get_const_env(1).get(1).value(),
      ConstantValue(0x1234567890ABCDEFL, ConstantValue::ConstantType::WIDE));

  // Block 2 -> [r0: 2, r1: 0x1234567890ABCDEFL]
  EXPECT_EQ(get_const_env(2).get(0).value(),
            ConstantValue(2, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(
      get_const_env(2).get(1).value(),
      ConstantValue(0x1234567890ABCDEFL, ConstantValue::ConstantType::WIDE));

  // Block 3 -> [r1: 0x1234567890ABCDEFL]
  EXPECT_EQ(
      get_const_env(3).get(1).value(),
      ConstantValue(0x1234567890ABCDEFL, ConstantValue::ConstantType::WIDE));
  EXPECT_TRUE(get_const_env(3).get(0).is_top());

  auto& stmt = m_program1.get(3)->stmts[0];
  EXPECT_TRUE(stmt.type == Statement::Type::WIDE_MOVE && stmt.dest == 3);

  constant_prop.simplify();

  // Now, make sure using these facts, we replaced the last assignment of r3
  // from a move into a load constant.
  EXPECT_TRUE(stmt.type == Statement::Type::WIDE_CONST && stmt.dest == 3 &&
              stmt.wide_value == 0x1234567890ABCDEF);
}

TEST_F(GlobalConstantPropagationTest, testProgram2) {
  SkeletonConstantPropAnalysis constant_prop(m_program2);
  constant_prop.run(ConstPropEnvironment());

  // Check constant facts in each every block
  auto get_const_env = [&](int id) {
    return constant_prop.get_constants_at_entry(m_program2.get(id));
  };

  // Block 0 -> Top
  EXPECT_TRUE(get_const_env(0).is_top());

  // Block 1 -> [r0: 0, r1: 0, r2: 10]
  EXPECT_EQ(get_const_env(1).get(0).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(1).get(1).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(1).get(2).value(),
            ConstantValue(10, ConstantValue::ConstantType::WIDE));

  // Block 2 -> [r0: 0, r2: 10]
  EXPECT_EQ(get_const_env(2).get(0).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(2).get(2).value(),
            ConstantValue(10, ConstantValue::ConstantType::WIDE));
  EXPECT_TRUE(get_const_env(2).get(1).is_top());

  // Block 3 -> [r0: 0, r2: 10]
  EXPECT_EQ(get_const_env(3).get(0).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(3).get(2).value(),
            ConstantValue(10, ConstantValue::ConstantType::WIDE));
  EXPECT_TRUE(get_const_env(3).get(1).is_top());

  // Block 4 -> [r0: 0, r2: 10]
  EXPECT_EQ(get_const_env(4).get(0).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(4).get(2).value(),
            ConstantValue(10, ConstantValue::ConstantType::WIDE));
  EXPECT_TRUE(get_const_env(4).get(1).is_top());

  // Block 5 -> [r0: 0, r2: 10]
  EXPECT_EQ(get_const_env(5).get(0).value(),
            ConstantValue(0, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(5).get(2).value(),
            ConstantValue(10, ConstantValue::ConstantType::WIDE));

  auto& stmt1 = m_program2.get(4)->stmts[0];
  EXPECT_TRUE(stmt1.type == Statement::Type::NARROW_MOVE && stmt1.dest == 1);
  auto& stmt2 = m_program2.get(5)->stmts[0];
  EXPECT_TRUE(stmt2.type == Statement::Type::NARROW_MOVE && stmt2.dest == 4);
  auto& stmt3 = m_program2.get(5)->stmts[1];
  EXPECT_TRUE(stmt3.type == Statement::Type::WIDE_MOVE && stmt3.dest == 5);

  constant_prop.simplify();

  EXPECT_TRUE(stmt1.type == Statement::Type::NARROW_CONST && stmt1.dest == 1 &&
              stmt1.narrow_value == 0);
  EXPECT_TRUE(stmt2.type == Statement::Type::NARROW_CONST && stmt2.dest == 4 &&
              stmt2.narrow_value == 0);
  EXPECT_TRUE(stmt3.type == Statement::Type::WIDE_CONST && stmt3.dest == 5 &&
              stmt3.wide_value == 10L);
}

TEST_F(GlobalConstantPropagationTest, testProgram3) {
  SkeletonConstantPropAnalysis constant_prop(m_program3);
  constant_prop.run(ConstPropEnvironment());

  // Check constant facts in each every block
  auto get_const_env = [&](int id) {
    return constant_prop.get_constants_at_entry(m_program3.get(id));
  };

  // Block 0 -> Top
  EXPECT_TRUE(get_const_env(0).is_top());

  // Block 1 -> [r3: 3]
  EXPECT_TRUE(get_const_env(1).get(0).is_top());
  EXPECT_TRUE(get_const_env(1).get(1).is_top());
  EXPECT_TRUE(get_const_env(1).get(2).is_top());
  EXPECT_EQ(get_const_env(1).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 2 -> [r0: 1, r2: 5, r3: 3]
  EXPECT_EQ(get_const_env(2).get(0).value(),
            ConstantValue(1, ConstantValue::ConstantType::WIDE));
  EXPECT_EQ(get_const_env(2).get(2).value(),
            ConstantValue(5, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(2).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 3 -> [r0: 1, r2: 5, r3: 3]
  EXPECT_EQ(get_const_env(3).get(0).value(),
            ConstantValue(1, ConstantValue::ConstantType::WIDE));
  EXPECT_EQ(get_const_env(3).get(2).value(),
            ConstantValue(5, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(3).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 4 -> [r3: 3]
  EXPECT_EQ(get_const_env(4).get(0).value(),
            ConstantValue(1, ConstantValue::ConstantType::WIDE));
  EXPECT_EQ(get_const_env(4).get(2).value(),
            ConstantValue(5, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(4).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 5 -> [r3: 3]
  EXPECT_TRUE(get_const_env(5).get(0).is_top());
  EXPECT_TRUE(get_const_env(5).get(1).is_top());
  EXPECT_TRUE(get_const_env(5).get(2).is_top());
  EXPECT_EQ(get_const_env(5).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 6 -> [r3: 3]
  EXPECT_TRUE(get_const_env(6).get(0).is_top());
  EXPECT_TRUE(get_const_env(6).get(1).is_top());
  EXPECT_TRUE(get_const_env(6).get(2).is_top());
  EXPECT_EQ(get_const_env(6).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 7 -> [r3: 3]
  EXPECT_TRUE(get_const_env(7).get(0).is_top());
  EXPECT_TRUE(get_const_env(7).get(1).is_top());
  EXPECT_TRUE(get_const_env(7).get(2).is_top());
  EXPECT_EQ(get_const_env(7).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  // Block 8 -> [r3: 3, r6: 3]
  EXPECT_TRUE(get_const_env(8).get(0).is_top());
  EXPECT_TRUE(get_const_env(8).get(1).is_top());
  EXPECT_TRUE(get_const_env(8).get(2).is_top());
  EXPECT_EQ(get_const_env(8).get(3).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));
  EXPECT_EQ(get_const_env(8).get(6).value(),
            ConstantValue(3, ConstantValue::ConstantType::NARROW));

  auto& stmt1 = m_program3.get(6)->stmts[0];
  EXPECT_TRUE(stmt1.type == Statement::Type::NARROW_MOVE && stmt1.dest == 6);

  auto& stmt2 = m_program3.get(7)->stmts[0];
  EXPECT_TRUE(stmt2.type == Statement::Type::NARROW_MOVE && stmt2.dest == 6);

  auto& stmt3 = m_program3.get(8)->stmts[0];
  EXPECT_TRUE(stmt3.type == Statement::Type::NARROW_MOVE && stmt3.dest == 7);

  constant_prop.simplify();

  EXPECT_TRUE(stmt1.type == Statement::Type::NARROW_CONST && stmt1.dest == 6 &&
              stmt1.narrow_value == 3);

  EXPECT_TRUE(stmt2.type == Statement::Type::NARROW_CONST && stmt2.dest == 6 &&
              stmt2.narrow_value == 3);

  EXPECT_TRUE(stmt3.type == Statement::Type::NARROW_CONST && stmt3.dest == 7 &&
              stmt3.narrow_value == 3);
}

TEST_F(GlobalConstantPropagationTest, testProgram4) {
  using namespace testing;

  SkeletonConstantPropAnalysis constant_prop(m_program4);
  constant_prop.run(ConstPropEnvironment());

  // Block 0 at entry -> Top
  EXPECT_TRUE(constant_prop.get_constants_at_entry(m_program4.get(0)).is_top());
  // Block 0 at exit -> [r1: 1]
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(0))
          .get(1)
          .value()
          .equals(ConstantValue(1, ConstantValue::ConstantType::NARROW)));

  // Block 1 at entry -> [r1: 1]
  EXPECT_TRUE(
      constant_prop.get_constants_at_entry(m_program4.get(1))
          .get(1)
          .value()
          .equals(ConstantValue(1, ConstantValue::ConstantType::NARROW)));
  // Block 1 at exit -> [r1: 1; r2:2; r3:1; r4:2]
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(1))
          .get(1)
          .value()
          .equals(ConstantValue(1, ConstantValue::ConstantType::NARROW)));
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(1))
          .get(2)
          .value()
          .equals(ConstantValue(2, ConstantValue::ConstantType::NARROW)));
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(1))
          .get(3)
          .value()
          .equals(ConstantValue(1, ConstantValue::ConstantType::NARROW)));
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(1))
          .get(4)
          .value()
          .equals(ConstantValue(2, ConstantValue::ConstantType::NARROW)));

  // Block 2 is unreachable. Both its entry and exit states are _|_.
  EXPECT_TRUE(
      constant_prop.get_constants_at_entry(m_program4.get(2)).is_bottom());
  EXPECT_TRUE(
      constant_prop.get_constants_at_exit(m_program4.get(2)).is_bottom());
}
