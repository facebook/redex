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
        (add-int v1 v0 v2)
        (const v3 -1)
        (add-int v3 v0 v3)
        (add-int v1 v0 v3)
        (const v4 2)
        (add-int v1 v0 v4)
        (return v1)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v2 1)
      (const v1 0)
      (add-int v0 v2 v1)
      (const v1 -1)
      (add-int v1 v2 v1)
      (add-int v0 v2 v1)
      (const v1 2)
      (add-int v0 v2 v1)
      (return v0)
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
      (const v1 1)
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
      (const v1 1)
      (const v0 1)
      (if-eqz v1 :branch)
      (return v1)

      (:branch)
      (add-int v1 v1 v0)
      (return v1)
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
      (const v3 10)
      (const v2 1)
      (:LHead)
      (if-gt v3 v2 :Loop)
      (add-int/lit8 v1 v3 1)
      (move v0 v1)
      (return v0)
      (:Loop)
      (add-int/lit8 v3 v3 -1)
      (goto :LHead)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Test behavior when there is wide arguments.
 */
TEST_F(FastRegAllocTest, WideVReg) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar:()J"
      (
        (const v2 1)
        (add-int/lit8 v3 v2 1)
        (const-wide v2 9223372036854775807)
        (return v3)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
        (const v1 1)
        (add-int/lit8 v0 v1 1)
        (const-wide v1 9223372036854775807)
        (return v0)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

/*
 * Check live analysis behavior when a vreg has multiple definitions in a basic
 * block. Later Def in the same block should not overwrite the first one. In one
 * block, any Def except the first one should not be recorded as a start/end
 * point anyway.
 */
TEST_F(FastRegAllocTest, ParamAlloc) {
  auto method = assembler::method_from_string(R"(
    (method (public static) "LUnexplainedConfig$3;.create:(LLacrimaConfig;)Ljava/lang/Object;"
      (
        (load-param-object v0)
        (load-param-object v1)
        (invoke-virtual (v1) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
        (move-result-object v2)
        (invoke-virtual (v1) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
        (move-result-object v3)
        (iget-object v3 "LSessionManager;.mProcessName:Ljava/lang/String;")
        (move-result-pseudo-object v5)
        (invoke-virtual (v2 v5) "LSessionManager;.getPreviousSessionDir:(Ljava/lang/String;)Ljava/io/File;")
        (move-result-object v2)
        (if-nez v2 :B2)
        (const v1 0)
        (return-object v1)
        (:B2)
        (invoke-virtual (v1) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
        (move-result-object v4)
        (invoke-virtual (v1) "LLacrimaConfig;.getForegroundEntityMapperProvider:()LProvider;")
        (move-result-object v1)
        (invoke-interface (v1) "LProvider;.get:()Ljava/lang/Object;")
        (move-result-object v1)
        (check-cast v1 "LForegroundEntityMapper;")
        (move-result-pseudo-object v1)
        (new-instance "LAppStateCollector;")
        (move-result-pseudo-object v3)
        (invoke-direct (v3 v2 v4 v1) "LAppStateCollector;.<init>:(Ljava/io/File;LSessionManager;LForegroundEntityMapper;)V")
        (return-object v3)
      )
    )
)");

  fastregalloc::LinearScanAllocator allocator(method);
  allocator.allocate();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v4)
      (load-param-object v3)
      (invoke-virtual (v3) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
      (move-result-object v2)
      (invoke-virtual (v3) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
      (move-result-object v1)
      (iget-object v1 "LSessionManager;.mProcessName:Ljava/lang/String;")
      (move-result-pseudo-object v0)
      (invoke-virtual (v2 v0) "LSessionManager;.getPreviousSessionDir:(Ljava/lang/String;)Ljava/io/File;")
      (move-result-object v2)
      (if-nez v2 :B2)
      (const v3 0)
      (return-object v3)
      (:B2)
      (invoke-virtual (v3) "LLacrimaConfig;.getSessionManager:()LSessionManager;")
      (move-result-object v0)
      (invoke-virtual (v3) "LLacrimaConfig;.getForegroundEntityMapperProvider:()LProvider;")
      (move-result-object v3)
      (invoke-interface (v3) "LProvider;.get:()Ljava/lang/Object;")
      (move-result-object v3)
      (check-cast v3 "LForegroundEntityMapper;")
      (move-result-pseudo-object v3)
      (new-instance "LAppStateCollector;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1 v2 v0 v3) "LAppStateCollector;.<init>:(Ljava/io/File;LSessionManager;LForegroundEntityMapper;)V")
      (return-object v1)
    )
)");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}
