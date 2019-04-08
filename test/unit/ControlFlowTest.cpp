/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"

namespace cfg {

std::ostream& operator<<(std::ostream& os, const Block* b) {
  return os << b->id();
}

} // namespace cfg

using namespace cfg;

TEST(ControlFlow, findExitBlocks) {
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b0})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b1})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b1);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b0, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b0})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    //   +---------+
    //   v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b0, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b2})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b2);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b1})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b1);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    //   |
    //   |
    //   v
    // +---+
    // | 3 |
    // +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_THAT(cfg.real_exit_blocks(/* include_infinite_loops */ true),
                ::testing::UnorderedElementsAre(b1, b3))
        << show(cfg);
    EXPECT_EQ(cfg.exit_block()->id(), 4);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 | --> | 3 |
    // +---+     +---+     +---+     +---+
    //   ^                             |
    //   +-----------------------------+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b2, b3, EDGE_GOTO);
    cfg.add_edge(b3, b0, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b0})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+
    //  +- | 0 | --> | 1 | --> | 2 |
    //  |  +---+     +---+     +---+
    //  |
    //  |    +---------+
    //  |    v         |
    //  |  +---+     +---+
    //  +> | 3 | --> | 4 |
    //     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_THAT(cfg.real_exit_blocks(/* include_infinite_loops */ true),
                ::testing::UnorderedElementsAre(b1, b3))
        << show(cfg);
    EXPECT_EQ(cfg.exit_block()->id(), 5);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+
    //  +- | 0 | --> | 1 | --> | 2 | --> | 5 |
    //  |  +---+     +---+     +---+     +---+
    //  |                                  ^
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.real_exit_blocks(/* include_infinite_loops */ true),
              std::vector<Block*>{b5})
        << show(cfg);
    EXPECT_EQ(cfg.exit_block(), b5);
  }
}

TEST(ControlFlow, findImmediateDominator) {
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+
    //  +- | 0 | --> | 1 | --> | 2 | --> | 5 |
    //  |  +---+     +---+     +---+     +---+
    //  |                                  ^
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    auto idom = cfg.immediate_dominators();
    EXPECT_EQ(idom[b0].dom, b0);
    EXPECT_EQ(idom[b1].dom, b0);
    EXPECT_EQ(idom[b3].dom, b0);
    EXPECT_EQ(idom[b2].dom, b1);
    EXPECT_EQ(idom[b4].dom, b3);
    EXPECT_EQ(idom[b5].dom, b0);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+
    //     | 0 | --> | 1 | --> | 2 | --> | 5 |
    //     +---+     +---+     +---+     +---+
    //                |                    ^
    //  +-------------+                    |
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b1, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    auto idom = cfg.immediate_dominators();
    EXPECT_EQ(idom[b0].dom, b0);
    EXPECT_EQ(idom[b1].dom, b0);
    EXPECT_EQ(idom[b3].dom, b1);
    EXPECT_EQ(idom[b2].dom, b1);
    EXPECT_EQ(idom[b4].dom, b3);
    EXPECT_EQ(idom[b5].dom, b1);
  }
}

TEST(ControlFlow, iterate1) {
  auto code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");
  code->build_cfg(/* editable */ true);
  for (Block* b : code->cfg().blocks()) {
    EXPECT_NE(nullptr, b);
  }
  for (const auto& mie : cfg::InstructionIterable(code->cfg())) {
    EXPECT_EQ(OPCODE_RETURN_VOID, mie.insn->opcode());
  }
}

TEST(ControlFlow, iterate2) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (:loop)
     (const v1 0)
     (if-gez v0 :if-true-label)
     (goto :loop) ; this goto is removed

     (:if-true-label)
     (return-void)
    )
)");
  code->build_cfg(/* editable */ true);
  for (Block* b : code->cfg().blocks()) {
    EXPECT_NE(nullptr, b);
  }

  // iterate within a block in the correct order
  // but visit the blocks in any order
  std::unordered_map<IRInstruction*, size_t> times_encountered;
  auto iterable = cfg::InstructionIterable(code->cfg());
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    EXPECT_FALSE(it.is_end());
    auto insn = it->insn;
    auto op = insn->opcode();
    if (op == OPCODE_CONST) {
      EXPECT_EQ(OPCODE_IF_GEZ, std::next(it)->insn->opcode());
    }
    times_encountered[insn] += 1;
  }
  EXPECT_TRUE(iterable.end().is_end());
  EXPECT_EQ(4, times_encountered.size());
  for (const auto& entry : times_encountered) {
    EXPECT_EQ(1, entry.second);
  }
  TRACE(CFG, 1, SHOW(code->cfg()));
}

// C++14 Null Forward Iterators
// Make sure the default constructed InstructionIterator compares equal
// to other default constructed InstructionIterator
//
// boost.org/doc/libs/1_58_0/doc/html/container/Cpp11_conformance.html
TEST(ControlFlow, nullForwardIterators) {
  auto code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  for (int i = 0; i < 100; i++) {
    auto a = new cfg::InstructionIterable(cfg);
    EXPECT_TRUE(a->end() == cfg::InstructionIterable(cfg).end());
    delete a;
  }

  IRList::iterator a;
  IRList::iterator b;
  EXPECT_EQ(a, b);
  for (int i = 0; i < 100; i++) {
    auto iterable = new cfg::InstructionIterable(cfg);
    EXPECT_EQ(a, iterable->end().unwrap());
    EXPECT_EQ(b, iterable->end().unwrap());
    delete iterable;
  }

  for (int i = 0; i < 100; i++) {
    auto iterator = new ir_list::InstructionIterator();
    EXPECT_EQ(ir_list::InstructionIterator(), *iterator);
    delete iterator;
  }

  auto iterable = cfg::InstructionIterable(cfg);
  EXPECT_TRUE(iterable.end().is_end());
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    EXPECT_FALSE(it.is_end());
  }
}

TEST(ControlFlow, copyConstructibleIterator) {
  auto code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
  )");
  code->build_cfg(/* editable */ true);
  ControlFlowGraph& cfg = code->cfg();
  auto ii = cfg::InstructionIterable(cfg);
  boost::optional<cfg::InstructionIterator> opt;
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    opt = it;
  }
}

TEST(ControlFlow, editableBuildAndLinearizeNoChange) {
  auto str = R"(
    (
      (const v0 0)
      (const v1 1)
      (move v3 v0)
      (return v3)
    )
  )";
  auto input_code = assembler::ircode_from_string(str);
  auto expected_code = assembler::ircode_from_string(str);

  input_code->build_cfg(/* editable */ true);
  input_code->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(input_code.get()))
      << "expected:\n"
      << show(expected_code) << "\n"
      << "actual:\n"
      << show(input_code) << "\n";
}

TEST(ControlFlow, infinite) {
  auto str = R"(
    (
      (:lbl)
      (goto :lbl)
    )
  )";
  auto input_code = assembler::ircode_from_string(str);
  auto expected_code = assembler::ircode_from_string(str);

  TRACE(CFG, 1, "%s", SHOW(input_code));
  input_code->build_cfg(/* editable */ true);
  TRACE(CFG, 1, "%s", SHOW(input_code->cfg()));
  input_code->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(input_code.get()))
      << "expected:\n"
      << show(expected_code) << "\n"
      << "actual:\n"
      << show(input_code) << "\n";
}

TEST(ControlFlow, infinite2) {
  auto str = R"(
    (
      (:lbl)
      (const v0 0)
      (goto :lbl)
    )
  )";
  auto input_code = assembler::ircode_from_string(str);
  auto expected_code = assembler::ircode_from_string(str);

  input_code->build_cfg(/* editable */ true);
  TRACE(CFG, 1, "%s", SHOW(input_code->cfg()));
  input_code->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(input_code.get()))
      << "expected:\n"
      << show(expected_code) << "\n"
      << "actual:\n"
      << show(input_code) << "\n";
}

TEST(ControlFlow, unreachable) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (:lbl)
      (return-void)

      (goto :lbl)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      ; cfg simplification removes the unreachable empty block
      (return-void)
    )
  )");

  input_code->build_cfg(/* editable */ true);
  TRACE(CFG, 1, "%s", SHOW(input_code->cfg()));
  input_code->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(input_code.get()))
      << "expected:\n"
      << show(expected_code) << "\n"
      << "actual:\n"
      << show(input_code) << "\n";
}

TEST(ControlFlow, unreachable2) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (:lbl)
      (return-void)

      (const v0 0)
      (goto :lbl)
    )
  )");
  auto expected_code = assembler::ircode_from_string(R"(
    (
      ; cfg simplification removes the unreachable block
      (return-void)
    )
  )");

  input_code->build_cfg(/* editable */ true);
  TRACE(CFG, 1, "%s", SHOW(input_code->cfg()));
  input_code->clear_cfg();

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(input_code.get()))
      << "expected:\n"
      << show(expected_code) << "\n"
      << "actual:\n"
      << show(input_code) << "\n";
}

TEST(ControlFlow, remove_non_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const-wide v2 1)
      (move v1 v0)
      (return-void)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::InstructionIterator> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST_WIDE) {
      to_delete.push_back(it);
    }
  }

  for (auto& it : to_delete) {
    cfg.remove_insn(it);
  }
  cfg.recompute_registers_size();

  code->clear_cfg();
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (move v1 v0)
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

void delete_if(ControlFlowGraph& cfg, std::function<bool(IROpcode)> predicate) {
  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::InstructionIterator> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (predicate(it->insn->opcode())) {
      to_delete.push_back(it);
    }
  }

  for (auto& it : to_delete) {
    cfg.remove_insn(it);
  }
  cfg.recompute_registers_size();
}

TEST(ControlFlow, remove_non_branch_with_loop) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     ; implicit goto (:loop)

     (:loop)
     (const v1 0)
     (if-gez v0 :if-true-label)
     (goto :loop)

     (:if-true-label)
     (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op == OPCODE_CONST; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     ; implicit goto :loop

     (:loop)
     (if-gez v0 :if-true-label)
     (goto :loop)

     (:if-true-label)
     (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :lbl)
      (const v1 1)

      (:lbl)
      (return-void)
    )
  )");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op == OPCODE_IF_EQZ; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_branch_with_loop) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (:loop)
     (const v1 0)
     (if-gez v0 :loop)

     (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op == OPCODE_IF_GEZ; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (const v1 0)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_all_but_return) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)

     (:loop)
     (const v1 0)
     (if-gez v0 :loop)

     (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op != OPCODE_RETURN_VOID; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v0 (:a :b))

      (:exit)
      (return-void)

      (:a 0)
      (const v0 0)
      (goto :exit)

      (:b 1)
      (const v1 1)
      (goto :exit)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op == OPCODE_SPARSE_SWITCH; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_switch2) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sparse-switch v0 (:a :b))
      (goto :exit)

      (:a 0)
      (const v0 0)
      (goto :exit)

      (:b 1)
      (const v1 1)
      (goto :exit)

      (:exit)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  delete_if(cfg, [](IROpcode op) { return op == OPCODE_SPARSE_SWITCH; });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_pred_edge_if) {
  auto code = assembler::ircode_from_string(R"(
    (
      (:a 0)
      (const v0 1)
      (if-eqz v0 :end)

      (sparse-switch v0 (:a :b))

      (:b 1)
      (const v0 2)
      (if-eqz v0 :end)

      (const v0 3)

      (:end)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  cfg.delete_pred_edge_if(cfg.entry_block(), [](const cfg::Edge* e) {
    return e->type() == EDGE_BRANCH;
  });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (if-eqz v0 :end)

      (sparse-switch v0 (:b))

      (:b 1)
      (const v0 2)
      (if-eqz v0 :end)

      (const v0 3)

      (:end)
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, cleanup_after_deleting_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (const v0 0)
      (goto :end)

      (:true)
      (const v1 1)

      (:end)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  cfg.delete_succ_edge_if(cfg.entry_block(), [](const cfg::Edge* e) {
    return e->type() == EDGE_BRANCH;
  });
  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, cleanup_after_deleting_goto) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (if-eqz v0 :true)

      (const v0 0)

      (:true)
      (const v1 1)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);

  auto& cfg = code->cfg();
  cfg.delete_succ_edge_if(cfg.entry_block(), [](const cfg::Edge* e) {
    return e->type() == EDGE_GOTO;
  });

  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 1)
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
}

TEST(ControlFlow, remove_sget) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (sget Lcom/Foo.bar:I)
      (move-result-pseudo v0)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::InstructionIterator> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == OPCODE_SGET) {
      to_delete.push_back(it);
    }
  }

  for (auto& it : to_delete) {
    cfg.remove_insn(it);
  }
  cfg.recompute_registers_size();

  code->clear_cfg();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code.get()));
  delete g_redex;
}

TEST(ControlFlow, branchingness) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "one")
      (move-result-pseudo v0)
      (if-eqz v0 :a)

      (const-string "two")
      (move-result-pseudo v0)
      (goto :end)

      (:a)
      (const-string "three")
      (move-result-pseudo v0)

      (:end)
      (const-string "four")
      (move-result-pseudo v0)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  uint16_t blocks_checked = 0;
  for (Block* b : cfg.blocks()) {
    std::string str = b->get_first_insn()->insn->get_string()->str();
    if (str == "one") {
      EXPECT_EQ(opcode::BRANCH_IF, b->branchingness());
      ++blocks_checked;
    }
    if (str == "two" || str == "three") {
      EXPECT_EQ(opcode::BRANCH_GOTO, b->branchingness());
      ++blocks_checked;
    }
    if (str == "four") {
      EXPECT_EQ(opcode::BRANCH_RETURN, b->branchingness());
      ++blocks_checked;
    }
  }
  EXPECT_EQ(4, blocks_checked);
  delete g_redex;
}

TEST(ControlFlow, empty_first_block) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (goto :exit)

      (add-int/lit8 v0 v0 1)

      (:exit)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::InstructionIterator> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST) {
      // make the first block empty
      to_delete.push_back(it);
    }
  }
  for (const auto& it : to_delete) {
    cfg.remove_insn(it);
  }
  cfg.recompute_registers_size();

  for (const auto& mie : cfg::ConstInstructionIterable(code->cfg())) {
    std::cout << show(mie) << std::endl;
  }
}

TEST(ControlFlow, exit_blocks) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :thr)
      (return-void)
      (:thr)
      (throw v0)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  EXPECT_EQ(2, cfg.real_exit_blocks().size());
  code->clear_cfg();
}

TEST(ControlFlow, exit_blocks_change) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :thr)
      (return-void)
      (:thr)
      (throw v0)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  EXPECT_EQ(2, cfg.real_exit_blocks().size());

  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::Block*> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == OPCODE_THROW) {
      to_delete.push_back(it.block());
    }
  }
  for (Block* b : to_delete) {
    cfg.remove_block(b);
  }
  cfg.recompute_registers_size();

  EXPECT_EQ(1, cfg.real_exit_blocks().size());
  code->clear_cfg();
}

TEST(ControlFlow, deep_copy1) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :thr)
      (return-void)
      (:thr)
      (throw v0)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& orig = code->cfg();

  cfg::ControlFlowGraph copy;
  orig.deep_copy(&copy);
  IRList* orig_list = orig.linearize();
  IRList* copy_list = copy.linearize();

  auto orig_iterable = ir_list::InstructionIterable(orig_list);
  auto copy_iterable = ir_list::InstructionIterable(copy_list);
  EXPECT_TRUE(orig_iterable.structural_equals(copy_iterable));
}

TEST(ControlFlow, deep_copy2) {
  g_redex = new RedexContext();

  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 10)

      (:loop)
      (if-eqz v0 :end)
      (invoke-static (v0) "LCls;.foo:(I)I")
      (move-result v1)
      (add-int v0 v0 v1)
      (goto :loop)

      (:end)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& orig = code->cfg();

  cfg::ControlFlowGraph copy;
  orig.deep_copy(&copy);
  IRList* orig_list = orig.linearize();
  IRList* copy_list = copy.linearize();

  auto orig_iterable = ir_list::InstructionIterable(orig_list);
  auto copy_iterable = ir_list::InstructionIterable(copy_list);
  EXPECT_TRUE(orig_iterable.structural_equals(copy_iterable));

  delete g_redex;
}

TEST(ControlFlow, deep_copy3) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 10)

      (:loop)
      (if-eqz v0 :end)

      (move v2 v0)
      (if-nez v2 :true)
      (const v2 0)
      (goto :inner_end)

      (:true)
      (const v2 -1)

      (:inner_end)
      (move v1 v2)

      (add-int v0 v0 v1)
      (goto :loop)

      (:end)
      (return-void)
    )
)");

  code->build_cfg(/* editable */ true);
  auto& orig = code->cfg();

  cfg::ControlFlowGraph copy;
  orig.deep_copy(&copy);
  IRList* orig_list = orig.linearize();
  IRList* copy_list = copy.linearize();

  auto orig_iterable = ir_list::InstructionIterable(orig_list);
  auto copy_iterable = ir_list::InstructionIterable(copy_list);
  EXPECT_TRUE(orig_iterable.structural_equals(copy_iterable));
}

TEST(ControlFlow, line_numbers) {
  g_redex = new RedexContext();

  DexMethod* m = static_cast<DexMethod*>(DexMethod::make_method("LFoo;.m:()V"));
  m->make_concrete(ACC_PUBLIC, /* is_virtual */ false);

  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos "LFoo;.m:()V" "Foo.java" 1)
      (if-eqz v0 :true)

      (const v1 1)
      (goto :exit)

      (:true)
      (const v2 2)

      (:exit)
      (.pos "LFoo;.m:()V" "Foo.java" 2)
      (return-void)
    )
  )");

  code->build_cfg(/* editable */ true);
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (.pos "LFoo;.m:()V" "Foo.java" 1)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (.pos "LFoo;.m:()V" "Foo.java" 2)
      (return-void)

      (:true)
      (.pos "LFoo;.m:()V" "Foo.java" 1)
      (const v2 2)
      (goto :exit)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));

  delete g_redex;
}

TEST(ControlFlow, simple_push_back) {
  ControlFlowGraph cfg{};
  Block* entry = cfg.create_block();
  cfg.set_entry_block(entry);
  entry->push_back(new IRInstruction(OPCODE_RETURN_VOID));
  cfg.sanity_check();
  for (Block* b : cfg.blocks()) {
    for (const auto& mie : ir_list::InstructionIterable(*b)) {
      always_assert(mie.insn->opcode() == OPCODE_RETURN_VOID);
    }
  }
}

TEST(ControlFlow, simple_push_front) {
  ControlFlowGraph cfg{};
  Block* entry = cfg.create_block();
  cfg.set_entry_block(entry);
  entry->push_front(new IRInstruction(OPCODE_RETURN_VOID));
  cfg.sanity_check();
  for (Block* b : cfg.blocks()) {
    for (const auto& mie : ir_list::InstructionIterable(*b)) {
      always_assert(mie.insn->opcode() == OPCODE_RETURN_VOID);
    }
  }
}

TEST(ControlFlow, insertion) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (return-void)

      (:true)
      (const v2 2)
      (goto :exit)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  IRInstruction add{OPCODE_ADD_INT_LIT8};
  add.set_literal(1);
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_const(insn->opcode())) {
      auto new_insn = new IRInstruction(add);
      new_insn->set_src(0, insn->dest());
      new_insn->set_dest(insn->dest());
      cfg.insert_after(it, new_insn);
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (add-int/lit8 v0 v0 1)
      (if-eqz v0 :true)

      (const v1 1)
      (add-int/lit8 v1 v1 1)

      (:exit)
      (return-void)

      (:true)
      (const v2 2)
      (add-int/lit8 v2 v2 1)
      (goto :exit)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
}

TEST(ControlFlow, add_sget) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (sput v1 "LFoo;.field:I")
      (return-void)

      (:true)
      (const v1 2)
      (goto :exit)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_conditional_branch(insn->opcode())) {
      IRInstruction* sget = new IRInstruction(OPCODE_SGET);
      sget->set_field(DexField::make_field("LFoo;.field:I"));
      IRInstruction* move_res = new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO);
      move_res->set_dest(insn->src(0));
      cfg.insert_before(it, {sget, move_res});
      break;
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (sget "LFoo;.field:I")
      (move-result-pseudo v0)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (sput v1 "LFoo;.field:I")
      (return-void)

      (:true)
      (const v1 2)
      (goto :exit)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
  delete g_redex;
}

TEST(ControlFlow, add_return) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (sput v1 "LFoo;.field:I")
      (return-void)

      (:true)
      (const v1 2)
      (goto :exit)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_conditional_branch(insn->opcode())) {
      auto ret = new IRInstruction(OPCODE_RETURN_VOID);
      cfg.insert_before(it, ret);
      break;
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (return-void)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
  delete g_redex;
}

TEST(ControlFlow, add_throw) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (.try_start foo)
      (const v0 0)
      (sget "LFoo;.field:I")
      (move-result-pseudo v0)
      (if-eqz v0 :true)

      (const v1 1)

      (:exit)
      (return v1)

      (:true)
      (const v1 2)
      (return v1)

      (.try_end foo)

      (.catch (foo))
      (const v1 3)
      (return v1)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_sget(insn->opcode())) {
      auto thr = new IRInstruction(OPCODE_THROW);
      cfg.insert_before(it, thr);
      break;
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (.try_start foo)
      (const v0 0)
      (throw v0)
      (.try_end foo)

      (.catch (foo))
      (const v1 3)
      (return v1)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
  delete g_redex;
}

TEST(ControlFlow, add_branch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (return v0)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  auto entry_block = cfg.entry_block();
  auto load_param =
      entry_block->to_cfg_instruction_iterator(*entry_block->begin());
  cfg.split_block(load_param);
  IRInstruction ret(OPCODE_RETURN);
  ret.set_src(0, 0);
  auto fls = cfg.create_block();
  {
    auto load_zero = new IRInstruction(OPCODE_CONST);
    load_zero->set_dest(0);
    load_zero->set_literal(0);
    fls->push_back({load_zero, new IRInstruction(ret)});
  }
  auto tru = cfg.create_block();
  {
    auto load_one = new IRInstruction(OPCODE_CONST);
    load_one->set_dest(0);
    load_one->set_literal(1);
    tru->push_back({load_one, new IRInstruction(ret)});
  }
  auto if_eqz = new IRInstruction(OPCODE_IF_EQZ);
  if_eqz->set_src(0, 0);
  cfg.create_branch(entry_block, if_eqz, fls, tru);
  cfg.recompute_registers_size();
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :tru)

      (const v0 0)
      (return v0)

      (:tru)
      (const v0 1)
      (return v0)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
}

Block* create_ret_const_block(ControlFlowGraph& cfg, uint64_t lit) {
  auto b = cfg.create_block();
  auto c = new IRInstruction(OPCODE_CONST);
  auto reg = cfg.allocate_temp();
  c->set_dest(reg);
  c->set_literal(lit);
  auto r = new IRInstruction(OPCODE_RETURN);
  r->set_src(0, reg);
  b->push_back({c, r});
  return b;
}

TEST(ControlFlow, add_branch_null_goto_block) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :tru)

      (const v1 10)
      (goto :exit)

      (:tru)
      (const v1 20)

      (:exit)
      (return v1)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  auto new_block = create_ret_const_block(cfg, 30);

  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST && it->insn->get_literal() == 10) {
      auto br = new IRInstruction(OPCODE_IF_LEZ);
      br->set_src(0, 0);
      cfg.create_branch(it.block(), br, nullptr, new_block);
      break;
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :tru)

      (const v1 10)
      (if-lez v0 :new_exit)

      (:exit)
      (return v1)

      (:tru)
      (const v1 20)
      (goto :exit)

      (:new_exit)
      (const v2 30)
      (return v2)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
}

TEST(ControlFlow, add_branch_redirect_goto_block) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :tru)

      (const v1 10)
      (goto :exit)

      (:tru)
      (const v1 20)

      (:exit)
      (return v1)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  auto thirty = create_ret_const_block(cfg, 30);
  auto forty = create_ret_const_block(cfg, 40);

  auto ii = cfg::InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST && it->insn->get_literal() == 10) {
      auto br = new IRInstruction(OPCODE_IF_LEZ);
      br->set_src(0, 0);
      cfg.create_branch(it.block(), br, thirty, forty);
      break;
    }
  }
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (if-eqz v0 :tru)

      (const v1 10)
      (if-lez v0 :forty)

      (const v2 30)
      (return v2)

      (:forty)
      (const v3 40)
      (return v3)

      (:tru)
      (const v1 20)
      (return v1)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
}

TEST(ControlFlow, add_switch) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (return v0)
    )
  )");
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();

  auto ten = create_ret_const_block(cfg, 10);
  auto twenty = create_ret_const_block(cfg, 20);
  auto thirty = create_ret_const_block(cfg, 30);
  auto forty = create_ret_const_block(cfg, 40);

  auto entry = cfg.entry_block();
  auto exit_block =
      cfg.split_block(entry->to_cfg_instruction_iterator(*entry->begin()));
  auto sw = new IRInstruction(OPCODE_PACKED_SWITCH);
  sw->set_src(0, 0);
  cfg.create_branch(cfg.entry_block(),
                    sw,
                    exit_block,
                    {{0, ten}, {1, twenty}, {2, thirty}, {3, forty}});
  code->clear_cfg();

  auto expected = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (packed-switch v0 (:ten :twenty :thirty :forty))
      (return v0)

      (:forty 3)
      (const v4 40)
      (return v4)

      (:thirty 2)
      (const v3 30)
      (return v3)

      (:twenty 1)
      (const v2 20)
      (return v2)

      (:ten 0)
      (const v1 10)
      (return v1)
    )
  )");
  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(code.get()));
}
