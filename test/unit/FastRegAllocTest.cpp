/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRAssembler.h"
#include "LinearScan.h"
#include "RedexTest.h"

struct FastRegAllocTest : public RedexTest {};

/*
 * Check function: allocate() in class: LinearScanAllocator
 * Note: current expected_code is based on the behavior of the non-spill
 * version.
 */
TEST_F(FastRegAllocTest, RegAlloc) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Z"
      (
        (const v0 1)
        (const v2 0)
        (cmp-long v1 v0 v2)
        (const v3 -1)
        (add-int v3 v0 v3)
        (cmp-long v1 v0 v3)
        (const v4 2)
        (cmp-long v1 v0 v4)
        (return v1)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 0)
      (cmp-long v2 v0 v1)
      (const v1 -1)
      (add-int v1 v0 v1)
      (cmp-long v2 v0 v1)
      (const v1 2)
      (cmp-long v2 v0 v1)
      (return v2)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Check allocation behavior when there is dead code (vreg defined but no use).
 * If a vreg is defined but never used, we assume its live interval lasts until
 * end of code.
 */
TEST_F(FastRegAllocTest, NoUseVReg) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Z"
      (
        (const v1 1)
        (const v3 2)
        (return v3)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 2)
      (return v1)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Check allocation behavior on control flow.
 */
TEST_F(FastRegAllocTest, ControlFlow) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Z"
      (
        (const v2 1)
        (const v1 1)
        (if-eqz v2 :branch)
        (return v2)

        (:branch)
        (add-int v2 v2 v1)
        (return v2)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (const v1 1)
      (if-eqz v0 :branch)
      (return v0)

      (:branch)
      (add-int v0 v0 v1)
      (return v0)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Check if input code is linearized.
 * Note : code may not be able to fully linearize, especially when there're
 * loops. See the next testcase.
 */
TEST_F(FastRegAllocTest, CheckCodeFlow) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Z"
      (
        (goto :def)

        (:use)
        (return v2)

        (:def)
        (const v2 3)
        (goto :use)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 3)
      (return v0)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Check allocation behavior when there're loops. Live interval endpoint of a
 * vreg in loop header can be neither a Use or a Def of the vreg.
 */
TEST_F(FastRegAllocTest, CheckVRegInLoop) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()Z"
      (
        (const v1 10)
        (const v2 1)
        (:LHead)
        (if-gt v1 v2 :Loop)
        (add-int/lit8 v3 v1 1)
        (move v0 v3)
        (return v0)
        (:Loop)
        (add-int/lit8 v1 v1 -1)
        (goto :LHead)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 10)
      (const v1 1)
      (:LHead)
      (if-gt v0 v1 :Loop)
      (add-int/lit8 v2 v0 1)
      (move v3 v2)
      (return v3)
      (:Loop)
      (add-int/lit8 v0 v0 -1)
      (goto :LHead)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}
