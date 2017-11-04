/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "CopyPropagationPass.h"
#include "IRAssembler.h"

using namespace copy_propagation_impl;

TEST(CopyPropagationTest, simple) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move v1 v0)
     (move v2 v1)
     (return v2)
    )
)");
  code->set_registers_size(3);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move v1 v0) ; these moves don't get deleted, but running localDCE after
     (move v2 v1) ; will clean them up
     (return v0)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, deleteRepeatedMove) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move-object v1 v0) ; this load doesn't get deleted, so that any reg
                         ; operands that cannot get remapped (like the
                         ; monitor-* instructions below) still remain valid

     (move-object v1 v0) ; this move can be deleted

     (monitor-enter v1) ; these won't be remapped to avoid breaking
                        ; ART verification
     (monitor-exit v1)
     (return v1)
    )
)");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move-object v1 v0)
     (monitor-enter v1)
     (monitor-exit v1)
     (return v0)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, noRemapRange) {
  g_redex = new RedexContext();

  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move-object v1 v0)

     ; v1 won't get remapped here because it's part of an instruction that
     ; will be converted to /range form during the lowering step
     (invoke-static (v1 v2 v3 v4 v5 v6) "LFoo;.bar:(IIIIII)V")

     (return v1)
    )
)");
  code->set_registers_size(7);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move-object v1 v0)
     (invoke-static (v1 v2 v3 v4 v5 v6) "LFoo;.bar:(IIIIII)V")
     (return v0)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));

  delete g_redex;
}
