/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexHasher.h"
#include "IRAssembler.h"
#include "RedexTest.h"

struct DexHasherTest : public RedexTest {};

TEST_F(DexHasherTest, DifferentRegistersMakeDifferentHash) {
  // Two methods differing only in register ID should have different hashes.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar1:()V"
      (
        (const v0 42)
        (return-void)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.bar2:()V"
      (
        (const v1 42)
        (return-void)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  auto hash1 = hashing::DexMethodHasher(method1).run();
  auto hash2 = hashing::DexMethodHasher(method2).run();

  EXPECT_NE(hash1.registers_hash, hash2.registers_hash);
}

TEST_F(DexHasherTest, MethodHashIgnoresClassAndMethodName) {
  // The hash should be the same even if the class and method names differ,
  // as long as the code is identical.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LClass1;.method1:()I"
      (
        (const v0 100)
        (return v0)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LClass2;.method2:()I"
      (
        (const v0 100)
        (return v0)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  auto hash1 = hashing::DexMethodHasher(method1).run();
  auto hash2 = hashing::DexMethodHasher(method2).run();

  EXPECT_EQ(hash1.code_hash, hash2.code_hash);
}

TEST_F(DexHasherTest, MethodHashDiffersForDifferentOpcodes) {
  // Methods with different opcodes should have different hashes.
  auto* method1 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.add:(II)I"
      (
        (load-param v0)
        (load-param v1)
        (add-int v2 v0 v1)
        (return v2)
      )
    )
  )");

  auto* method2 = assembler::method_from_string(R"(
    (method (public static) "LFoo;.sub:(II)I"
      (
        (load-param v0)
        (load-param v1)
        (sub-int v2 v0 v1)
        (return v2)
      )
    )
  )");

  method1->get_code()->build_cfg();
  method2->get_code()->build_cfg();

  auto hash1 = hashing::DexMethodHasher(method1).run();
  auto hash2 = hashing::DexMethodHasher(method2).run();

  EXPECT_NE(hash1.code_hash, hash2.code_hash);
}
