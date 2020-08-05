/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "StripDebugInfo.h"

using namespace strip_debug_info_impl;

// Currently the only dbg info that's supported by IRAssembler is basic
// positions (by basic I mean positions without parents).

struct StripDebugInfoTest : public RedexTest {};

void test(const StripDebugInfoPass::Config& config,
          const char* i,
          const char* o) {
  auto code = assembler::ircode_from_string(i);
  code->set_registers_size(3);
  StripDebugInfo(config).run(*code);
  auto expected_code = assembler::ircode_from_string(o);
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(StripDebugInfoTest, noopWithoutDebugInfo) {
  StripDebugInfoPass::Config config;
  config.drop_all_dbg_info = true;
  test(config,
       R"(
    (
     (const v0 0)
     (return v0)
    )
)",
       R"(
    (
     (const v0 0)
     (return v0)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersWithThrowing) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.baz:I"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC);
  StripDebugInfoPass::Config config;
  config.drop_line_nrs = true;
  test(config,
       R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (sget "LFoo;.baz:I")
     (move-result-pseudo v0)
    )
)",
       R"(
    (
     (sget "LFoo;.baz:I")
     (move-result-pseudo v0)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersWithNonThrowing) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  StripDebugInfoPass::Config config;
  config.drop_line_nrs = true;
  test(config,
       R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
    )
)",
       R"(
    (
     (const v0 420)
    )
)");
}
