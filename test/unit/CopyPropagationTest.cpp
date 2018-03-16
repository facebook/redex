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
     ; these moves don't get deleted, but running DCE after will clean them up
     (move v1 v0) ; this makes v0 the representative for v1
     ; this source register is remapped by replace_with_representative
     (move v2 v0)
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

TEST(CopyPropagationTest, deleteSelfMove) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v1 0)
      (move v0 v0)
    )
)");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v1 0)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, representative) {
  g_redex = new RedexContext();
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (move v1 v0)
      (invoke-static (v0) "Lcls;.foo:(I)V")
      (invoke-static (v1) "Lcls;.bar:(I)V")
    )
)");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (move v1 v0)
      (invoke-static (v0) "Lcls;.foo:(I)V")
      (invoke-static (v0) "Lcls;.bar:(I)V")
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
  delete g_redex;
}

TEST(CopyPropagationTest, verifyEnabled) {
  // assuming verify-none is disabled for this test
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (int-to-float v1 v0) ; use v0 as float
      (const v0 0)
      (float-to-int v1 v0) ; use v0 as int
    )
)");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (int-to-float v1 v0) ; use v0 as float
      (const v0 0) ; DON'T delete this. Verifier needs it
      (float-to-int v1 v0) ; use v0 as int
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, cliqueAliasing) {
  auto code = assembler::ircode_from_string(R"(
    (
      (move v1 v2)
      (move v0 v1)
      (move v1 v3)
      (move v0 v2)
    )
  )");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  config.replace_with_representative = false;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (move v1 v2)
      (move v0 v1)
      (move v1 v3)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, loopNoChange) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 10)

      (:loop)
      (if-eq v0 v1 :end)
      (add-int/lit8 v0 v0 1)
      (goto :loop)

      (:end)
      (return-void)
    )
  )");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 10)

      (:loop)
      (if-eq v0 v1 :end)
      (add-int/lit8 v0 v0 1)
      (goto :loop)

      (:end)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, branchNoChange) {
  auto code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v3 v2)

      (:end)
      (move v1 v3)
      (return-void)
    )
  )");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v3 v2)

      (:end)
      (move v1 v3)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, intersect1) {
  auto code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v1 v2)

      (:end)
      (move v1 v2)
      (return-void)
    )
  )");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v1 v2)

      (:end)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, intersect2) {
  auto no_change = R"(
    (
      (move v0 v1)
      (if-eqz v0 :true)

      (move v3 v1)
      (goto :end)

      (:true)
      (move v4 v1)

      (:end)
      (move v3 v4)
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(no_change);
  code->set_registers_size(5);

  CopyPropagationPass::Config config;
  config.replace_with_representative = false;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(no_change);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, wide) {
  auto code = assembler::ircode_from_string(R"(
    (
      (move-wide v0 v2)
      (move-wide v0 v2)
    )
  )");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  config.wide_registers = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (move-wide v0 v2)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, wideClobber) {
  auto code = assembler::ircode_from_string(R"(
    (
      (move v1 v4)
      (move-wide v0 v2)
      (move v1 v4)
    )
  )");
  code->set_registers_size(5);

  CopyPropagationPass::Config config;
  config.wide_registers = false;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (move v1 v4)
      (move-wide v0 v2)
      (move v1 v4)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, wideClobberWideTrue) {
  auto code = assembler::ircode_from_string(R"(
    (
      (move v1 v4)
      (move-wide v0 v2)
      (move v1 v4)
    )
  )");
  code->set_registers_size(5);

  CopyPropagationPass::Config config;
  config.wide_registers = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (move v1 v4)
      (move-wide v0 v2)
      (move v1 v4)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, repWide) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 0)
      (move-wide v2 v0)
      (const v1 0)
      (move-wide v4 v2)
    )
  )");
  code->set_registers_size(5);

  CopyPropagationPass::Config config;
  config.wide_registers = true;
  config.replace_with_representative = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 0)
      (move-wide v2 v0)
      (const v1 0)
      (move-wide v4 v2) ; don't switch v2 to v0
                        ; because `const v1` invalidated v0
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// whichRep and whichRep2 make sure that we deterministically choose the
// representative after a merge point.
TEST(CopyPropagationTest, whichRep) {
  auto no_change = R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v2 v1)

      (:end)
      (move v3 v1)
    )
  )";
  auto code = assembler::ircode_from_string(no_change);
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  config.replace_with_representative = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(no_change);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, whichRep2) {
  auto no_change = R"(
    (
      (if-eqz v0 :true)

      (move v2 v1)
      (goto :end)

      (:true)
      (move v1 v2)

      (:end)
      (move v3 v1)
    )
  )";
  auto code = assembler::ircode_from_string(no_change);
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  config.replace_with_representative = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(no_change);
}

// make sure we keep using the oldest representative even after a merge
TEST(CopyPropagationTest, whichRepPreserve) {
  auto code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v1 v2)

      (:end)
      (move v3 v1)
    )
  )");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  config.replace_with_representative = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (if-eqz v0 :true)

      (move v1 v2)
      (goto :end)

      (:true)
      (move v1 v2)

      (:end)
      (move v3 v2)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST(CopyPropagationTest, wideInvokeSources) {
  g_redex = new RedexContext();

  auto no_change = R"(
    (
      (move-wide v0 v15)
      (invoke-static (v0) "Lcom;.foo:(J)V")
    )
  )";
  auto code = assembler::ircode_from_string(no_change);
  code->set_registers_size(16);

  CopyPropagationPass::Config config;
  config.replace_with_representative = true;
  config.wide_registers = true;
  config.regalloc_has_run = true;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(no_change);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));

  delete g_redex;
}
