/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRList.h"
#include "InstructionLowering.h"
#include "RedexTest.h"

namespace {

struct RemarkFields {
  std::string producer;
  std::string val_str;
  int64_t val_int;
};

// Collect all remarks in an IRList (linear).
std::vector<RemarkFields> remarks_of(const IRCode* code) {
  std::vector<RemarkFields> out;
  for (const auto& mie : *code) {
    if (mie.type == MFLOW_REMARK) {
      out.push_back({mie.remark->producer->str_copy(),
                     mie.remark->val_str->str_copy(), mie.remark->val_int});
    }
  }
  return out;
}

} // namespace

struct RemarkTest : public RedexTest {};

// The assembler round-trips a remark through serialize -> parse -> serialize.
// Covers the MIE union/ctor, to_s_expr, and remark_from_s_expr.
TEST_F(RemarkTest, roundTrip) {
  auto s = std::string(
      "((.remark \"SomePass\" \"Dummy\" 42) "
      "(.remark \"SomePass\" \"Other\" -7) (return-void))");
  auto code = assembler::ircode_from_string(s);

  auto remarks = remarks_of(code.get());
  ASSERT_EQ(remarks.size(), 2u);
  EXPECT_EQ(remarks[0].producer, "SomePass");
  EXPECT_EQ(remarks[0].val_str, "Dummy");
  EXPECT_EQ(remarks[0].val_int, 42);
  EXPECT_EQ(remarks[1].producer, "SomePass");
  EXPECT_EQ(remarks[1].val_str, "Other");
  EXPECT_EQ(remarks[1].val_int, -7);

  EXPECT_EQ(assembler::to_string(code.get()),
            assembler::to_string(assembler::ircode_from_string(s).get()));
}

// The IRCode copy ctor (deep_copy_ir_list, the non-CFG "trap" path) must
// deep-copy the remark payload.
TEST_F(RemarkTest, deepCopyLinear) {
  auto code = assembler::ircode_from_string(
      "((.remark \"SomePass\" \"Dummy\" 42) (return-void))");
  // The copy-construction is exactly the behavior under test (the linear
  // deep_copy_ir_list path), so the "unnecessary copy" is intentional here.
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  IRCode copy(*code);

  auto remarks = remarks_of(&copy);
  ASSERT_EQ(remarks.size(), 1u);
  EXPECT_EQ(remarks[0].producer, "SomePass");
  EXPECT_EQ(remarks[0].val_str, "Dummy");
  EXPECT_EQ(remarks[0].val_int, 42);

  // The original is unaffected (independent payloads).
  ASSERT_EQ(remarks_of(code.get()).size(), 1u);
}

// ControlFlowGraph::deep_copy uses MethodItemEntryCloner (the primary CFG
// clone site). The remark must be carried into the copy.
TEST_F(RemarkTest, deepCopyCfg) {
  auto code = assembler::ircode_from_string(
      "((.remark \"SomePass\" \"Dummy\" 42) (const v0 0) (return-void))");
  code->build_cfg();

  cfg::ControlFlowGraph new_cfg;
  code->cfg().deep_copy(&new_cfg);

  size_t count = 0;
  for (auto* block : new_cfg.blocks()) {
    for (const auto& mie : *block) {
      if (mie.type == MFLOW_REMARK) {
        ++count;
        EXPECT_EQ(mie.remark->producer->str_copy(), "SomePass");
        EXPECT_EQ(mie.remark->val_str->str_copy(), "Dummy");
        EXPECT_EQ(mie.remark->val_int, 42);
      }
    }
  }
  EXPECT_EQ(count, 1u);
}

// A remark survives a CFG build -> linearize round trip unchanged.
TEST_F(RemarkTest, cfgRoundTrip) {
  auto code = assembler::ircode_from_string(
      "((.remark \"SomePass\" \"Dummy\" 42) (const v0 0) (return-void))");
  code->build_cfg();
  code->clear_cfg();

  auto remarks = remarks_of(code.get());
  ASSERT_EQ(remarks.size(), 1u);
  EXPECT_EQ(remarks[0].producer, "SomePass");
  EXPECT_EQ(remarks[0].val_str, "Dummy");
  EXPECT_EQ(remarks[0].val_int, 42);
}

// Remarks are redex-internal: neither string is gathered into the dex string
// pool, and sync strips the remark MIEs before emission.
TEST_F(RemarkTest, neverSerialized) {
  auto* method = assembler::method_from_string(
      "(method (public) \"LFoo;.bar:()V\" "
      "((.remark \"SourceNeverSerialized\" \"StrNeverSerialized\" 7) "
      "(return-void)))");
  auto* code = method->get_code();

  std::vector<const DexString*> strings;
  code->gather_strings(strings);
  EXPECT_EQ(std::find(strings.begin(), strings.end(),
                      DexString::make_string("SourceNeverSerialized")),
            strings.end());
  EXPECT_EQ(std::find(strings.begin(), strings.end(),
                      DexString::make_string("StrNeverSerialized")),
            strings.end());

  instruction_lowering::lower(method);
  code->sync(method);

  size_t remaining = 0;
  for (const auto& mie : *code) {
    if (mie.type == MFLOW_REMARK) {
      ++remaining;
    }
  }
  EXPECT_EQ(remaining, 0u);
}
