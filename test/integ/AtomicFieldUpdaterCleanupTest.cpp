/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "AtomicFieldUpdaterLoweringPass.h"
#include "AtomicFieldUpdaters.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"

namespace {

DexClass* get_class(const char* desc) {
  auto* type = DexType::get_type(desc);
  return type == nullptr ? nullptr : type_class(type);
}

size_t count_static_fields_of_type(DexClass* cls, const char* desc) {
  auto* type = DexType::get_type(desc);
  size_t count = 0;
  for (auto* field : cls->get_sfields()) {
    if (field->get_type() == type) {
      count++;
    }
  }
  return count;
}

size_t count_clinit_invokes_named(DexClass* cls, const char* name) {
  auto* clinit = cls->get_clinit();
  auto* code = clinit == nullptr ? nullptr : clinit->get_code();
  if (code == nullptr) {
    return 0;
  }
  cfg::ScopedCFG scoped(code);
  size_t count = 0;
  const auto* dex_name = DexString::get_string(name);
  for (auto& mie : cfg::InstructionIterable(*scoped)) {
    auto* insn = mie.insn;
    if (insn->has_method() && insn->get_method()->get_name() == dex_name) {
      count++;
    }
  }
  return count;
}

size_t count_clinit_field_refs_to_owner(DexClass* cls, const char* owner_desc) {
  auto* clinit = cls->get_clinit();
  auto* code = clinit == nullptr ? nullptr : clinit->get_code();
  if (code == nullptr) {
    return 0;
  }
  auto* owner = DexType::get_type(owner_desc);
  cfg::ScopedCFG scoped(code);
  size_t count = 0;
  for (auto& mie : cfg::InstructionIterable(*scoped)) {
    auto* insn = mie.insn;
    if (insn->has_field() && insn->get_field()->get_class() == owner) {
      count++;
    }
  }
  return count;
}

} // namespace

class AtomicFieldUpdaterCleanupIntegTest : public RedexIntegrationTest {
 protected:
  void run() {
    std::vector<Pass*> passes{new AtomicFieldUpdaterLoweringPass()};
    run_passes(passes);
  }
};

TEST_F(AtomicFieldUpdaterCleanupIntegTest, cleansUpPerHolderState) {
  run();

  auto* fully = get_class(
      "Lcom/facebook/redextest/"
      "AtomicFieldUpdaterCleanup$FullyRewrittenHolder;");
  ASSERT_NE(fully, nullptr);
  EXPECT_EQ(
      count_static_fields_of_type(fully, atomic_field_updaters::REFERENCE_DESC),
      0);
  EXPECT_EQ(count_static_fields_of_type(fully, "J"), 1);
  EXPECT_EQ(count_clinit_invokes_named(fully, "newUpdater"), 0);

  auto* partial = get_class(
      "Lcom/facebook/redextest/"
      "AtomicFieldUpdaterCleanup$PartiallyRewrittenHolder;");
  ASSERT_NE(partial, nullptr);
  EXPECT_EQ(count_static_fields_of_type(partial,
                                        atomic_field_updaters::REFERENCE_DESC),
            1);
  EXPECT_EQ(count_static_fields_of_type(partial, "J"), 1);
  EXPECT_EQ(count_clinit_invokes_named(partial, "newUpdater"), 1);
  EXPECT_EQ(count_clinit_invokes_named(partial, "getDeclaredField"), 1);
  EXPECT_EQ(count_clinit_invokes_named(partial, "objectFieldOffset"), 1);

  auto* unrewritten = get_class(
      "Lcom/facebook/redextest/AtomicFieldUpdaterCleanup$UnrewrittenHolder;");
  ASSERT_NE(unrewritten, nullptr);
  EXPECT_EQ(count_static_fields_of_type(unrewritten,
                                        atomic_field_updaters::REFERENCE_DESC),
            1);
  EXPECT_EQ(count_static_fields_of_type(unrewritten, "J"), 0);
  EXPECT_EQ(count_clinit_invokes_named(unrewritten, "newUpdater"), 1);
  EXPECT_EQ(count_clinit_invokes_named(unrewritten, "getDeclaredField"), 0);
  EXPECT_EQ(count_clinit_invokes_named(unrewritten, "objectFieldOffset"), 0);
  EXPECT_EQ(count_clinit_field_refs_to_owner(
                unrewritten, atomic_field_updaters::SYNTH_HOLDER_DESC),
            0);
}
