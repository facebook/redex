/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexAsm.h"
#include "IRAssembler.h"
#include "IRInstruction.h"
#include "RedexTest.h"

#include <functional>
#include <iterator>

using namespace cfg;
using namespace dex_asm;
using namespace opcode;

namespace {

/// Test that the mutation to the control flow graph representation of \p actual
/// results in the \p expected IR.
///
/// \p m A function that mutates the CFG given to it.
/// \p actual The actual state of the IR before the mutation has been applied,
///     as an s-expression.
/// \p expected The expected state of the IR after the mutation has been
///     applied, as an s-expression.
void EXPECT_MUTATION(const std::function<void(IRCode&, ControlFlowGraph&)>& m,
                     const char* actual,
                     const char* expected) {
  auto actual_ir = assembler::ircode_from_string(actual);
  const auto expected_ir = assembler::ircode_from_string(expected);

  actual_ir->build_cfg(/* editable */ true);

  // Run body of test (that performs the mutation).
  m(*actual_ir, actual_ir->cfg());

  // The mutation may introduce more register uses, so recompute them.
  actual_ir->cfg().recompute_registers_size();

  actual_ir->clear_cfg();
  EXPECT_CODE_EQ(expected_ir.get(), actual_ir.get());
}

class CFGMutationTest : public RedexTest {};

TEST_F(CFGMutationTest, RemoveAllButLastPositon) {
  EXPECT_MUTATION([](IRCode&, ControlFlowGraph& cfg) { cfg.simplify(); },
                  /* ACTUAL */ R"((
        (.pos:dbg_0 method_name RedexGenerated 0)
        (.pos:dbg_1 method_name RedexGenerated 0)
        (.pos:dbg_2 method_name RedexGenerated 0)
        (const v0 0)
        (return-void)
      ))",
                  /* EXPECTED */ R"((
        (.pos:dbg_0 method_name RedexGenerated 0)
        (const v0 0)
        (return-void)
      ))");
}

TEST_F(CFGMutationTest, SimplificationRemovesEmptyBlockWithPosition) {
  EXPECT_MUTATION([](IRCode&, ControlFlowGraph& cfg) { cfg.simplify(); },
                  /* ACTUAL */ R"((
        (.pos:dbg_parent method_name RedexGenerated 0)
        (goto :Loop)

        (:Loop)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))",
                  /* EXPECTED */ R"((
        (:Loop)
        (.pos:dbg_parent method_name RedexGenerated 0)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))");
}

TEST_F(CFGMutationTest, RetainParentWhenRemovingBlock) {
  EXPECT_MUTATION(
      [](IRCode&, ControlFlowGraph& cfg) {
        cfg.remove_block(cfg.blocks().at(0));
      },
      /* ACTUAL */ R"((
        (.pos:dbg_parent method_name RedexGenerated 0)
        (const v0 0)
        (goto :Loop)

        (:Loop)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))",
      /* EXPECTED */ R"((
        (:Loop)
        (.pos:dbg_parent method_name RedexGenerated 0)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))");
}

TEST_F(CFGMutationTest, RetainParentWhenReplacingBlock) {
  EXPECT_MUTATION(
      [](IRCode&, ControlFlowGraph& cfg) {
        cfg.replace_block(cfg.blocks().at(0), cfg.create_block());
      },
      /* ACTUAL */ R"((
        (.pos:dbg_parent method_name RedexGenerated 0)
        (const v0 0)
        (goto :Loop)

        (:Loop)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))",
      /* EXPECTED */ R"((
        (:Loop)
        (.pos:dbg_parent method_name RedexGenerated 0)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))");
}

TEST_F(CFGMutationTest, RetainParentsWhenRemovingBlock) {
  EXPECT_MUTATION(
      [](IRCode&, ControlFlowGraph& cfg) {
        cfg.remove_block(cfg.blocks().at(0));
      },
      /* ACTUAL */ R"((
        (.pos:dbg_parent_parent method_name RedexGenerated 0)
        (.pos:dbg_parent method_name RedexGenerated 0 dbg_parent_parent)
        (const v0 0)
        (goto :Loop)

        (:Loop)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))",
      /* EXPECTED */ R"((
        (:Loop)
        (.pos:dbg_parent_parent method_name RedexGenerated 0)
        (.pos:dbg_parent method_name RedexGenerated 0 dbg_parent_parent)
        (.pos:dbg_child method_name RedexGenerated 0 dbg_parent)
        (const v0 0)
        (.pos:dbg_child method_name RedexGenerated 1 dbg_parent)
        (goto :Loop)
      ))");
}

TEST_F(CFGMutationTest, RemoveDuplicatePositions) {
  EXPECT_MUTATION(
      [](IRCode& code, ControlFlowGraph&) {
        // rebuilder cfg
        code.build_cfg();
      },
      /* ACTUAL */ R"((
        (.pos:pos0 FooBar Foo.java 0)
        (const v0 0)
        (.pos:pos1 FooBar Foo.java 0)
        (.pos:pos2 FooBar Foo.java 0)
        (.pos:pos3 FooBar Foo.java 0)
        (return-void)
      ))",
      /* EXPECTED */ R"((
        (.pos:pos4 FooBar Foo.java 0)
        (const v0 0)
        (return-void)
      ))");
}

TEST_F(CFGMutationTest, RemoveRedundantPositions) {
  EXPECT_MUTATION(
      [](IRCode& code, ControlFlowGraph&) {
        // rebuilder cfg
        code.build_cfg();
      },
      /* ACTUAL */ R"((
        (.pos:pos0 FooBar Foo.java 0)
        (const v0 0)
        (.pos:pos1 FooBar Foo.java 1)
        (.pos:pos2 FooBar Foo.java 2)
        (.pos:pos3 FooBar Foo.java 3)
        (return-void)
      ))",
      /* EXPECTED */ R"((
        (.pos:pos0 FooBar Foo.java 0)
        (const v0 0)
        (.pos:pos3 FooBar Foo.java 3)
        (return-void)
      ))");
}

TEST_F(CFGMutationTest, RemoveRedundantPositionsButKeepParents) {
  EXPECT_MUTATION(
      [](IRCode& code, ControlFlowGraph&) {
        // rebuilder cfg
        code.build_cfg();
      },
      /* ACTUAL */ R"((
        (.pos:pos0parent FooBoo Foo.java 0)
        (.pos:pos0 FooBar Foo.java 100 pos0parent)
        (const v0 0)
        (.pos:pos1 FooBar Foo.java 101)
        (.pos:pos2 FooBar Foo.java 102)
        (.pos:pos3parent FooBoo Foo.java 0)
        (.pos:pos3 FooBar Foo.java 103 pos3parent)
        (return-void)
      ))",
      /* EXPECTED */ R"((
        (.pos:pos0parent FooBoo Foo.java 0)
        (.pos:pos0 FooBar Foo.java 100 pos0parent)
        (const v0 0)
        (.pos:pos3parent FooBoo Foo.java 0)
        (.pos:pos3 FooBar Foo.java 103 pos3parent)
        (return-void)
      ))");
}
TEST_F(CFGMutationTest, RemoveRedundantAndDuplicatePositions) {
  EXPECT_MUTATION(
      [](IRCode& code, ControlFlowGraph&) {
        // rebuilder cfg
        code.build_cfg();
      },
      /* ACTUAL */ R"((
        (.pos:pos0 FooBar Foo.java 0)
        (const v0 0)
        (.pos:pos1 FooBar Foo.java 1)
        (.pos:pos2 FooBar Foo.java 2)
        (.pos:pos3 FooBar Foo.java 0)
        (return-void)
      ))",
      /* EXPECTED */ R"((
        (.pos:pos0 FooBar Foo.java 0)
        (const v0 0)
        (return-void)
      ))");
}

TEST_F(CFGMutationTest, RedundantAndDuplicatePositionsWithParents) {
  // Unfortunately, the logic in remove_redundant_positions cannot yet collapse
  // this.
  EXPECT_MUTATION(
      [](IRCode& code, ControlFlowGraph&) {
        // rebuild cfg
        code.build_cfg();
      },
      /* ACTUAL */ R"((
        (.pos:pos0parent FooBoo Foo.java 0)
        (.pos:pos0 FooBar Foo.java 100 pos0parent)
        (const v0 0)
        (.pos:pos1parent FooBoo Foo.java 0)
        (.pos:pos1 FooBar Foo.java 100 pos1parent)
        (return-void)
      ))",
      /* EXPECTED */ R"((
        (.pos:pos0parent FooBoo Foo.java 0)
        (.pos:pos0 FooBar Foo.java 100 pos0parent)
        (const v0 0)
        (.pos:pos1parent FooBoo Foo.java 0)
        (.pos:pos1 FooBar Foo.java 100 pos1parent)
        (return-void)
      ))");
}

} // namespace
