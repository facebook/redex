/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexLoader.h"
#include "VerifyUtil.h"

namespace {
// We use this ugly macro expansion instead of loops for better gtest reporting.
// (Name, Expected # of code-unit reduction)
//
// when computing the total number of code units, write it this order:
// (how many times optimization runs) * (code units saved per run)
//
// NOTE: d8 8.9 runs its own StringBuilderOptimizer over explicit
// StringBuilder chains (folding `new StringBuilder().append(x)` into
// `new StringBuilder(x)`, dropping `append("")`, coalescing constant
// appends), so the input dex no longer contains those patterns and the
// Redex pass has nothing left to save. The entries below stay wired at 0
// to keep loading the methods; only the ValueOf patterns still produce
// Redex-side savings.
#define TESTS                                       \
  WORK(test_Coalesce_InitVoid_AppendString, 0)      \
  WORK(test_Remove_AppendEmptyString, 0)            \
  WORK(test_Coalesce_Init_AppendChar, 0)            \
  WORK(test_Coalesce_AppendString_AppendInt, 0)     \
  WORK(test_Coalesce_AppendString_AppendChar, 0)    \
  WORK(test_Coalesce_AppendString_AppendBoolean, 0) \
  WORK(test_Coalesce_AppendString_AppendLongInt, 0) \
  WORK(test_Replace_ValueOfBoolean, 2 * 2)          \
  WORK(test_Replace_ValueOfChar, 4 * 2)             \
  WORK(test_Replace_ValueOfInt, 8 * 2)              \
  WORK(test_Replace_ValueOfLongInt, 5 * 2)          \
  WORK(test_Replace_ValueOfFloat, 3 * 2)            \
  WORK(test_Replace_ValueOfDouble, 3 * 2)

void load_method_sizes(DexClasses& classes,
                       std::unordered_map<std::string, int>& map) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/SimplifyString;");
  ASSERT_NE(nullptr, cls);

#define WORK(name, ...)                                   \
  {                                                       \
    auto method_##name = find_vmethod_named(*cls, #name); \
    ASSERT_NE(nullptr, method_##name);                    \
    map[#name] = method_##name->get_dex_code()->size();   \
  }
  TESTS
#undef WORK
}
} // namespace

struct PrePostVerify : testing::Test {
  std::unordered_map<std::string, int> before_sizes;
  std::unordered_map<std::string, int> after_sizes;

  PrePostVerify() {
    g_redex = new RedexContext;
    DexClasses before_classes(load_classes_from_dex(
        DexLocation::make_location("", std::getenv("dex_pre")),
        /*stats=*/nullptr,
        /*input_dex_version*/ nullptr,
        /* balloon */ false));
    load_method_sizes(before_classes, before_sizes);
    delete g_redex;

    g_redex = new RedexContext;
    DexClasses after_classes(load_classes_from_dex(
        DexLocation::make_location("", std::getenv("dex_post")),
        /*stats=*/nullptr,
        /*input_dex_version*/ nullptr,
        /* balloon */ false));
    load_method_sizes(after_classes, after_sizes);
    delete g_redex;

    g_redex = nullptr;
  }

  ~PrePostVerify() {}
};

// To verify whether Redex replaced the patterns successfully, we compute the
// differences of the before/after methods.
//
// We check that the savings are at least the size of the difference in the
// peephole patterns, instead of exactly equal to it, because other
// transformations / optimizations may further shrink the dex file.
TEST_F(PrePostVerify, CheckSizes) {
#define WORK(name, saving)                                       \
  {                                                              \
    auto diff_##name = before_sizes[#name] - after_sizes[#name]; \
    constexpr int expected_saving = saving;                      \
    EXPECT_LE(expected_saving, diff_##name);                     \
  }
  TESTS
#undef WORK
}
