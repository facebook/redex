/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexPosition.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"
#include "SourceDebugExtension.h"

class DexPositionTest : public RedexTest {};

TEST_F(DexPositionTest, multiplePositionBeforeOpcode) {
  auto* method = DexMethod::make_method("LFoo;.bar:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (.pos "LFoo;.bar:()V" "Foo.java" 124)
      (const v0 0)
      (return-void)
    )
  )");
  code->set_debug_item(std::make_unique<DexDebugItem>());
  method->set_code(std::move(code));

  instruction_lowering::lower(method);
  method->sync();
  method->balloon();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 124)
      (const v0 0)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(DexPositionTest, consecutiveIdenticalPositions) {
  auto* method = DexMethod::make_method("LFoo;.bar:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (return-void)
    )
  )");
  code->set_debug_item(std::make_unique<DexDebugItem>());
  method->set_code(std::move(code));

  instruction_lowering::lower(method);
  method->sync();
  method->balloon();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (const v0 0)
      (return-void)
    )
  )");

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(DexPositionTest, sourceDebugExtensionBuildsNestedCallerChain) {
  auto extension = source_debug_extension::SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Caller.kt
2 Inline.kt
*L
27#2:41
22#2:42
*S KotlinDebug
*F
1 Caller.kt
*L
33#1:41
33#1:42
*E
)");
  ASSERT_TRUE(extension);

  auto* method = DexMethod::make_method("LFoo;.bar:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  DexDebugItem debug_item;
  std::vector<DexDebugEntry> entries;
  entries.emplace_back(0, std::make_unique<DexPosition>(
                              DexString::make_string("Generated.kt"), 41));
  entries.emplace_back(1, std::make_unique<DexPosition>(
                              DexString::make_string("Generated.kt"), 42));
  debug_item.set_entries(std::move(entries));

  debug_item.bind_positions(method, DexString::make_string("Generated.kt"),
                            &*extension);

  const auto& mapped_entries = debug_item.get_entries();
  ASSERT_EQ(mapped_entries.size(), 3);
  const auto* callsite = mapped_entries[0].pos.get();
  const auto* outer_inline = mapped_entries[1].pos.get();
  const auto* inner_inline = mapped_entries[2].pos.get();
  EXPECT_EQ(callsite->file->str(), "Caller.kt");
  EXPECT_EQ(callsite->line, 33);
  EXPECT_EQ(callsite->parent, nullptr);
  EXPECT_EQ(outer_inline->file->str(), "Inline.kt");
  EXPECT_EQ(outer_inline->line, 27);
  EXPECT_EQ(outer_inline->parent, callsite);
  EXPECT_EQ(inner_inline->file->str(), "Inline.kt");
  EXPECT_EQ(inner_inline->line, 22);
  EXPECT_EQ(inner_inline->parent, outer_inline);

  DexDebugItem copied(debug_item);
  const auto& copied_entries = copied.get_entries();
  ASSERT_EQ(copied_entries.size(), 3);
  EXPECT_EQ(copied_entries[1].pos->parent, copied_entries[0].pos.get());
  EXPECT_EQ(copied_entries[2].pos->parent, copied_entries[1].pos.get());
}

TEST_F(DexPositionTest, sourceDebugExtensionSharesCallsitePosition) {
  auto extension = source_debug_extension::SourceDebugExtension::parse(R"(SMAP
Generated.kt
Kotlin
*S Kotlin
*F
1 Inline.kt
*L
10#1,2:100
*S KotlinDebug
*F
1 Caller.kt
*L
42#1:100,2
*E
)");
  ASSERT_TRUE(extension);

  auto* method = DexMethod::make_method("LFoo;.bar:()V")
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  DexDebugItem debug_item;
  std::vector<DexDebugEntry> entries;
  entries.emplace_back(0, std::make_unique<DexPosition>(
                              DexString::make_string("Generated.kt"), 100));
  entries.emplace_back(1, std::make_unique<DexPosition>(
                              DexString::make_string("Generated.kt"), 101));
  debug_item.set_entries(std::move(entries));

  debug_item.bind_positions(method, DexString::make_string("Generated.kt"),
                            &*extension);

  const auto& mapped_entries = debug_item.get_entries();
  ASSERT_EQ(mapped_entries.size(), 3);
  ASSERT_EQ(mapped_entries[0].type, DexDebugEntryType::Position);
  ASSERT_EQ(mapped_entries[1].type, DexDebugEntryType::Position);
  ASSERT_EQ(mapped_entries[2].type, DexDebugEntryType::Position);
  EXPECT_EQ(mapped_entries[0].addr, 0);
  EXPECT_EQ(mapped_entries[1].addr, 0);
  EXPECT_EQ(mapped_entries[2].addr, 1);

  const auto* callsite = mapped_entries[0].pos.get();
  EXPECT_EQ(callsite->file->str(), "Caller.kt");
  EXPECT_EQ(callsite->line, 42);
  EXPECT_EQ(mapped_entries[1].pos->parent, callsite);
  EXPECT_EQ(mapped_entries[2].pos->parent, callsite);
  EXPECT_EQ(mapped_entries[1].pos->file->str(), "Inline.kt");
  EXPECT_EQ(mapped_entries[1].pos->line, 10);
  EXPECT_EQ(mapped_entries[2].pos->file->str(), "Inline.kt");
  EXPECT_EQ(mapped_entries[2].pos->line, 11);

  DexDebugItem copied(debug_item);
  const auto& copied_entries = copied.get_entries();
  ASSERT_EQ(copied_entries.size(), 3);
  EXPECT_EQ(copied_entries[1].pos->parent, copied_entries[0].pos.get());
  EXPECT_EQ(copied_entries[2].pos->parent, copied_entries[0].pos.get());
}
