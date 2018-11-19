/**
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

void test(StripDebugInfoPass::Config config, const char* i, const char* o) {
  auto code = assembler::ircode_from_string(i);
  code->set_registers_size(3);
  StripDebugInfo(config).run(*code);
  EXPECT_EQ(assembler::to_string(assembler::ircode_from_string(o).get()),
            assembler::to_string(code.get()));
}

TEST_F(StripDebugInfoTest, noopWithoutDebugInfo) {
  test(
      {
          .drop_all_dbg_info = true,
      },
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

  test(
      {
          .drop_line_nrs = true,
      },
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

  test(
      {
          .drop_line_nrs = true,
      },
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

TEST_F(StripDebugInfoTest, dropLineNumbersPreSafeWithThrowing) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto field = static_cast<DexField*>(DexField::make_field("LFoo;.baz:I"));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC);

  test(
      {
          .drop_line_nrs_preceeding_safe = true,
      },
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (sget "LFoo;.baz:I")
     (move-result-pseudo v0)
    )
)",
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (sget "LFoo;.baz:I")
     (move-result-pseudo v0)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersPreSafeWithNonThrowingNotOnly) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  test(
      {
          .drop_line_nrs_preceeding_safe = true,
      },
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
    )
)",
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersPreSafeWithNonThrowingNotFirst) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  test(
      {
          .drop_line_nrs_preceeding_safe = true,
      },
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
     (.pos "LFoo;.bar:()V" "Foo.java" "421")
     (const v1 421)
    )
)",
      R"(
    (
     (.pos "LFoo;.bar:()V" "Foo.java" "420")
     (const v0 420)
     (const v1 421)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersPreSafeNopAfterInlined) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()I"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:()I"));
  method2->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  test(
      {
          .drop_line_nrs_preceeding_safe = true,
      },
      R"(
    (
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v1 420)
     (.pos "LFoo;.bar:()I" "Foo.java" 420)
     (const v0 420)
     (.pos "LFoo;.baz:()I" "Foo.java" 12 0)
     (const v1 432)
     (.pos "LFoo;.bar:()I" "Foo.java" 421)
     (add-int v0 v1 v1)
     (return v1)
    )
)",
      R"(
    (
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v1 420)
     (.pos "LFoo;.bar:()I" "Foo.java" 420)
     (const v0 420)
     (.pos "LFoo;.baz:()I" "Foo.java" 12 0)
     (const v1 432)
     (.pos "LFoo;.bar:()I" "Foo.java" 421)
     (add-int v0 v1 v1)
     (return v1)
    )
)");
}

TEST_F(StripDebugInfoTest, dropLineNumbersPreSafeDontStripEventuallyThrowy) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()I"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method2 =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.baz:()I"));
  method2->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto method3 =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.quz:()I"));
  method3->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  test(
      {
          .drop_line_nrs_preceeding_safe = true,
      },
      R"(
    (
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v1 420)
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v0 420)
     (sget "LFoo;.qux:I")
    )
)",
      R"(
    (
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v1 420)
     (.pos "LFoo;.bar:()I" "Foo.java" 419)
     (const v0 420)
     (sget "LFoo;.qux:I")
    )
)");
}
