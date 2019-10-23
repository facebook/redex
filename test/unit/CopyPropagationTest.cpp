/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CopyPropagationPass.h"
#include "IRAssembler.h"
#include "RedexTest.h"

using namespace copy_propagation_impl;

class CopyPropagationTest : public RedexTest {};

TEST_F(CopyPropagationTest, simple) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, deleteRepeatedMove) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, noRemapRange) {

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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, deleteSelfMove) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, representative) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, verifyEnabled) {
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
      (int-to-float v1 v0) ; use v0 as int
      (const v0 0) ; DON'T delete this. Verifier needs it
      (float-to-int v1 v0) ; use v0 as float
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, consts_safe_by_constant_uses) {
  // even with verify-none being disabled, the following is safe
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (int-to-float v1 v0) ; use v0 as int
      (const v0 0)
      (int-to-double v1 v0) ; use v0 as int
    )
)");
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (int-to-float v1 v0) ; use v0 as int
      (int-to-double v1 v0) ; use v0 as int
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, consts_safe_by_constant_uses_aput) {

  // even with verify-none being disabled, the following is safe

  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 0)
      (new-array v0 "[I")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const v2 0) ; can be deleted
      (aput v0 v1 v2)
      (const v0 0) ; can be deleted
      (int-to-double v1 v0)
     )
    )
)");
  auto code = method->get_code();
  code->set_registers_size(3);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (new-array v0 "[I")
      (move-result-pseudo-object v1)
      (const v2 0) ; dead, and local-dce would delete later
      (aput v0 v1 v0)
      (int-to-double v1 v0)
    )
)");

  EXPECT_CODE_EQ(code, expected_code.get());
}

TEST_F(CopyPropagationTest, consts_unsafe_by_constant_uses_aput) {

  // the following is not safe, and shall not be fully optimized
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
      (const v0 0)
      (new-array v0 "[F") ; array of float
      (move-result-pseudo-object v1)
      (const v0 0) ; used as float
      (const v2 0) ; used as int
      (aput v0 v1 v2)
      (const v0 0) ; used as int
      (int-to-double v1 v0)
     )
    )
)");
  auto code = method->get_code();
  code->set_registers_size(3);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (new-array v0 "[F") ; array of float
      (move-result-pseudo-object v1)
      (const v0 0) ; used as float
      (const v2 0) ; used as int
      (aput v0 v1 v2)
      (const v0 0) ; used as int, redundant with v2!
      (int-to-double v1 v2)
      )
)");

  EXPECT_CODE_EQ(code, expected_code.get());
}

TEST_F(CopyPropagationTest, wide_consts_safe_by_constant_uses) {
  // even with verify-none being disabled, the following is safe
  auto code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 0)
      (long-to-float v2 v0) ; use v0 as long
      (const-wide v0 0)
      (long-to-double v2 v0) ; use v0 as long
    )
)");
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v0 0)
      (long-to-float v2 v0) ; use v0 as long
      (long-to-double v2 v0) ; use v0 as long
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, if_constraints_with_constant_uses) {

  // if-eq and if-ne require that *both* of their incoming registers agree on
  // either being an object reference, or an integer.
  // This provides for further refinement of constant uses, allowing to
  // copy-propagate in more cases (but also disallowing in others).
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()V"
     (
       (const v0 0)
       (const v2 0)
       (new-array v2 "[I")
       (move-result-pseudo-object v1)
       (if-eq v0 v1 :somewhere)

       (const v4 0)
       (move-object v3 v4) ; can be rewritten to refer to v0 instead of v4
       (return-object v3) ; can be rewritten to refer to v0 instead of v3

       (:somewhere)
       (return-object v1)
     )
    )
)");
  auto code = method->get_code();
  code->set_registers_size(4);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v2 0)
      (new-array v2 "[I")
      (move-result-pseudo-object v1)
      (if-eq v0 v1 :somewhere)

      (const v4 0)
      (move-object v3 v0)
      (return-object v0)

      (:somewhere)
      (return-object v1)
    )
)");

  EXPECT_CODE_EQ(code, expected_code.get());
}

TEST_F(CopyPropagationTest, cliqueAliasing) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, loopNoChange) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, branchNoChange) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, intersect1) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, intersect2) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, wide) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, wideClobber) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, wideClobberWideTrue) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, repWide) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// whichRep and whichRep2 make sure that we deterministically choose the
// representative after a merge point.
TEST_F(CopyPropagationTest, whichRep) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, whichRep2) {
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
TEST_F(CopyPropagationTest, whichRepPreserve) {
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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, wideInvokeSources) {

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

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(CopyPropagationTest, use_does_not_kill_type_demands) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Ljava/lang/Object;"
     (
       (const v0 0)
       (monitor-enter v0)
       (monitor-exit v0)
       (const v0 0) ; can be deleted
       (return-object v0)
    )
  )
)");
  auto code = method->get_code();
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (monitor-enter v0)
      (monitor-exit v0)
      (return-object v0)
    )
)");

  EXPECT_CODE_EQ(code, expected_code.get());
}

TEST_F(CopyPropagationTest, instance_of_kills_type_demands) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Ljava/lang/Object;"
     (
       (const v0 0)
       (instance-of v0 "Ljava/lang/String;")
       (move-result-pseudo v1)
       (const v0 0) ; can not be deleted
       (return-object v0)
    )
  )
)");
  auto code = method->get_code();
  code->set_registers_size(2);

  CopyPropagationPass::Config config;
  CopyPropagation(config).run(code, method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (instance-of v0 "Ljava/lang/String;")
      (move-result-pseudo v1)
      (const v0 0) ; can not be deleted
      (return-object v0)
    )
)");

  EXPECT_CODE_EQ(code, expected_code.get());
}
