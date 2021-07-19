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
 * If a vreg is defined but never used, we assume its live interval to be 1
 * insn.
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
      (const v0 2)
      (return v0)
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
