/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

std::ostream& operator<<(std::ostream& os, const ControlFlowGraph& cfg) {
  return cfg.write_dot_format(os);
}

}

using namespace cfg;

TEST(ControlFlow, findExitBlocks) {
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    cfg.set_entry_block(b0);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b1}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b1);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b0, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b2}) << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b1}) << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_THAT(find_exit_blocks(cfg), ::testing::UnorderedElementsAre(b1, b3))
        << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_THAT(find_exit_blocks(cfg), ::testing::UnorderedElementsAre(b1, b3))
        << cfg;
    cfg.calculate_exit_block();
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
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b5}) << cfg;
    cfg.calculate_exit_block();
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
  code->build_cfg(true);
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
  code->build_cfg(true);
  for (Block* b : code->cfg().blocks()) {
    EXPECT_NE(nullptr, b);
  }

  // iterate within a block in the correct order
  // but visit the blocks in any order
  std::unordered_map<IRInstruction*, size_t> times_encountered;
  auto iterable = cfg::InstructionIterable(code->cfg());
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    auto insn = it->insn;
    auto op = insn->opcode();
    if (op == OPCODE_CONST) {
      EXPECT_EQ(OPCODE_IF_GEZ, std::next(it)->insn->opcode());
    }
    times_encountered[insn] += 1;
  }
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
  code->build_cfg(true);
  auto& cfg = code->cfg();
  for (int i = 0; i < 100; i++) {
    auto a = new cfg::InstructionIterable(cfg);
    EXPECT_TRUE(a->end() == cfg::InstructionIterable(cfg).end());
    delete a;
  }

  IRList::iterator a;
  IRList::iterator b;
  EXPECT_TRUE(a == b);
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

  input_code->build_cfg(true);
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
  input_code->build_cfg(true);
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

  input_code->build_cfg(true);
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

  input_code->build_cfg(true);
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

  input_code->build_cfg(true);
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
      (move v1 v2)
      (return-void)
    )
  )");
  code->build_cfg(true);
  auto& cfg = code->cfg();

  auto iterable = cfg::InstructionIterable(cfg);
  std::vector<cfg::InstructionIterator> to_delete;
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn->opcode() == OPCODE_CONST_WIDE) {
      to_delete.push_back(it);
    }
  }

  for (auto& it : to_delete) {
    cfg.remove_opcode(it);
  }

  code->clear_cfg();
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (move v1 v2)
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
    cfg.remove_opcode(it);
  }
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

  code->build_cfg(true);
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

  code->build_cfg(true);
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

  code->build_cfg(true);
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

  code->build_cfg(true);
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

  code->build_cfg(true);
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

  code->build_cfg(true);
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

  code->build_cfg(true);
  auto& cfg = code->cfg();
  cfg.remove_pred_edge_if(cfg.entry_block(),
                          [](const cfg::Edge* e) {
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
