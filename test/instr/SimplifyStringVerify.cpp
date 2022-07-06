/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "IRCode.h"
#include "Show.h"
#include "VerifyUtil.h"
#include "Walkers.h"

namespace {
// We use this ugly macro expansion instead of loops for better gtest reporting.
// (Name, Expected # of code-unit reduction)
//
// when computing the total number of code units, write it this order:
// (how many times optimization runs) * (code units saved per run)
#define TESTS                                           \
  WORK(test_Coalesce_InitVoid_AppendString, 3)          \
  WORK(test_CompileTime_StringHashCode, 5 * 1)          \
  WORK(test_Remove_AppendEmptyString, 1 * 3)            \
  WORK(test_Coalesce_Init_AppendChar, 4)                \
  WORK(test_Coalesce_AppendString_AppendInt, 6 * 1)     \
  WORK(test_Coalesce_AppendString_AppendChar, 6 * 1)    \
  WORK(test_Coalesce_AppendString_AppendBoolean, 2 * 1) \
  WORK(test_Coalesce_AppendString_AppendLongInt, 4 * 1) \
  WORK(test_Replace_ValueOfBoolean, 2 * 2)              \
  WORK(test_Replace_ValueOfChar, 4 * 2)                 \
  WORK(test_Replace_ValueOfInt, 8 * 2)                  \
  WORK(test_Replace_ValueOfLongInt, 5 * 2)              \
  WORK(test_Replace_ValueOfFloat, 3 * 2)                \
  WORK(test_Replace_ValueOfDouble, 3 * 2)

void load_method_sizes(DexClasses& classes,
                       std::unordered_map<std::string, int>& map) {
  auto cls = find_class_named(classes,
                              "Lcom/facebook/redex/test/instr/SimplifyString;");
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
    DexClasses before_classes(
        load_classes_from_dex(std::getenv("dex_pre"), /* balloon */ false));
    load_method_sizes(before_classes, before_sizes);
    delete g_redex;

    g_redex = new RedexContext;
    DexClasses after_classes(
        load_classes_from_dex(std::getenv("dex_post"), /* balloon */ false));
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
