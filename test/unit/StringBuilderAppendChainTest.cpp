/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringBuilderAppendChain.h"

#include <functional>
#include <string>
#include <string_view>

#include <boost/algorithm/string/replace.hpp>
#include <gtest/gtest.h>

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "IROpcode.h"
#include "InstructionAnalyzer.h"
#include "JarLoader.h"
#include "RedexTest.h"

namespace cp = constant_propagation;

namespace {
using StringAnalyzer =
    InstructionAnalyzerCombiner<cp::StringAnalyzer, cp::PrimitiveAnalyzer>;
} // namespace

class StringBuilderAppendChainTest : public RedexTest {
 public:
  void SetUp() override {
    std::string sdk_jar = android_sdk_jar_path();
    // The analysis and the nullness check look up java.lang.StringBuilder /
    // java.lang.String and their methods, so the SDK JAR must be loaded.
    ASSERT_TRUE(load_jar_file(DexLocation::make_location("", sdk_jar)));
  }

 protected:
  // Builds an intraprocedural constant-propagation fixpoint that proves
  // const-string operands non-null (StringAnalyzer) and values non-null on the
  // continuation of a dereference/guard (the no-throw analyzer), then applies
  // the reduction and returns the number of sites rewritten. This is the same
  // shape of fixpoint IPConstantPropagation hands to the reduction, minus the
  // interprocedural WholeProgramState seeding.
  size_t run_transform(
      DexMethod* method,
      const std::function<size_t(const cp::intraprocedural::FixpointIterator&,
                                 cfg::ControlFlowGraph&)>& transform) {
    auto* code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto string_state = cp::StringAnalyzerState::make_default();
    cp::NullCheckMethods null_check_methods;
    cp::intraprocedural::FixpointIterator fp_iter(
        cfg, StringAnalyzer(&string_state, nullptr),
        cp::intraprocedural::make_default_no_throw_analyzer(
            &null_check_methods));
    fp_iter.run(ConstantEnvironment());
    size_t n = transform(fp_iter, cfg);
    code->clear_cfg();
    return n;
  }

  size_t run(DexMethod* method) {
    return run_transform(method,
                         constant_propagation::stringbuilder_append_chain::
                             reduce_two_append_concats);
  }

  size_t run_merge(DexMethod* method) {
    return run_transform(method,
                         constant_propagation::stringbuilder_append_chain::
                             merge_adjacent_constant_appends);
  }

  // Both reductions over one CFG and one fixpoint, in the order
  // InterproceduralConstantPropagationPass runs them. The concat reduction
  // therefore replays a fixpoint solved before the merge edited the CFG, as it
  // does in the pass. Returns {appends merged, sites concatenated}.
  std::pair<size_t, size_t> run_merge_then_concat(DexMethod* method) {
    size_t merged = 0;
    size_t reduced = run_transform(
        method, [&merged](const cp::intraprocedural::FixpointIterator& fp_iter,
                          cfg::ControlFlowGraph& cfg) {
          merged = constant_propagation::stringbuilder_append_chain::
              merge_adjacent_constant_appends(fp_iter, cfg);
          return constant_propagation::stringbuilder_append_chain::
              reduce_two_append_concats(fp_iter, cfg);
        });
    return {merged, reduced};
  }
};

/*
 * The canonical shape: the toString() invoke becomes a String.concat of the two
 * appended values, reading them from the temps captured at each append. The
 * builder, its constructor and its appends remain -- nothing reads the builder
 * afterwards, but removing a dead allocation is a later dead-code pass's job.
 */
TEST_F(StringBuilderAppendChainTest, twoNonNullStringAppendsBecomeConcat) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "b")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  EXPECT_EQ(run(method), 1);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (move-object v3 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "b")
      (move-result-pseudo-object v2)
      (move-object v4 v2)
      (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v3 v4) "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

/*
 * A builder's contents are fixed at each append, so a write to an operand's
 * register between the append and the toString() must not change the result.
 * Copying each operand into a fresh temp at its append is what keeps concat
 * reading the value that was appended rather than the register's later
 * contents.
 */
TEST_F(StringBuilderAppendChainTest,
       reassignedOperandStillConcatsAppendedValue) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "b")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "clobber")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  EXPECT_EQ(run(method), 1);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (move-object v3 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "b")
      (move-result-pseudo-object v2)
      (move-object v4 v2)
      (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "clobber")
      (move-result-pseudo-object v1)
      (invoke-virtual (v3 v4) "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

struct NullableOperandCase {
  // Instructions defining an appended value. An empty string leaves the
  // register holding the incoming parameter, whose nullness the analysis
  // cannot establish.
  std::string_view first_operand;
  std::string_view second_operand;
  std::string_view name;
};

/*
 * String.concat dereferences its receiver and its argument, so the reduction
 * requires both appended values to be provably non-null. Each case leaves a
 * different subset of the operands as bare parameters.
 */
class NullableOperandTest
    : public StringBuilderAppendChainTest,
      public ::testing::WithParamInterface<NullableOperandCase> {};

TEST_P(NullableOperandTest, notReduced) {
  const auto* body_template = R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v3)
        (load-param-object v4)
        FIRST_OPERAND
        SECOND_OPERAND
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0 v4) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )";
  std::string body = body_template;
  boost::replace_all(body, "FIRST_OPERAND", GetParam().first_operand);
  boost::replace_all(body, "SECOND_OPERAND", GetParam().second_operand);
  auto* method = assembler::method_from_string(body);

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

INSTANTIATE_TEST_SUITE_P(
    NullableOperandTests,
    NullableOperandTest,
    ::testing::Values(
        NullableOperandCase{
            "", R"((const-string "b") (move-result-pseudo-object v4))",
            "firstOperandNullable"},
        NullableOperandCase{
            R"((const-string "a") (move-result-pseudo-object v3))", "",
            "secondOperandNullable"},
        NullableOperandCase{"", "", "bothOperandsNullable"}),
    [](const testing::TestParamInfo<NullableOperandTest::ParamType>& info) {
      return std::string(info.param.name);
    });

struct AppendTypeCase {
  // Instructions defining and appending one operand.
  std::string_view first_append;
  std::string_view second_append;
  std::string_view name;
};

constexpr std::string_view kStringAppend =
    R"((const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;"))";

constexpr std::string_view kIntAppend =
    R"((const v2 42)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(I)Ljava/lang/StringBuilder;"))";

/*
 * String.concat takes and returns Strings, so an append of any other modeled
 * type keeps the builder. Each case gives a different subset of the operands a
 * non-String type.
 */
class AppendTypeTest : public StringBuilderAppendChainTest,
                       public ::testing::WithParamInterface<AppendTypeCase> {};

TEST_P(AppendTypeTest, notReduced) {
  const auto* body_template = R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        FIRST_APPEND
        SECOND_APPEND
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )";
  std::string body = body_template;
  boost::replace_all(body, "FIRST_APPEND", GetParam().first_append);
  boost::replace_all(body, "SECOND_APPEND", GetParam().second_append);
  auto* method = assembler::method_from_string(body);

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

INSTANTIATE_TEST_SUITE_P(
    AppendTypeTests,
    AppendTypeTest,
    ::testing::Values(
        AppendTypeCase{kIntAppend, kStringAppend, "firstAppendNotString"},
        AppendTypeCase{kStringAppend, kIntAppend, "secondAppendNotString"},
        AppendTypeCase{kIntAppend, kIntAppend, "bothAppendsNotString"}),
    [](const testing::TestParamInfo<AppendTypeTest::ParamType>& info) {
      return std::string(info.param.name);
    });

/*
 * Three appends are not transformed, and are left to StringBuilderOutlinerPass.
 */
TEST_F(StringBuilderAppendChainTest, threeAppendsNotReduced) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "b")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "c")
        (move-result-pseudo-object v3)
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * A value dereferenced before the append (here via a method call with it as the
 * receiver) is non-null on the continuation, which the fixpoint records as NEZ.
 * This is the intraprocedural stand-in for the interprocedural case IPCP adds:
 * a parameter proven non-null across all callers is likewise seeded NEZ.
 */
TEST_F(StringBuilderAppendChainTest, dereferencedParamsReduced) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v1)
        (load-param-object v2)
        (invoke-virtual (v1) "Ljava/lang/String;.length:()I")
        (move-result v3)
        (invoke-virtual (v2) "Ljava/lang/String;.length:()I")
        (move-result v4)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  EXPECT_EQ(run(method), 1);
}
/*
 * Two adjacent constant-String appends in a mixed builder merge into one append
 * of the concatenation; the leading non-constant append is left in place.
 */
TEST_F(StringBuilderAppendChainTest, twoAdjacentConstantAppendsMerge) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v3)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  EXPECT_EQ(run_merge(method), 1);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v3)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "a")
      (move-result-pseudo-object v1)
      (const-string "ab")
      (move-result-pseudo-object v4)
      (invoke-virtual (v0 v4) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "b")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

/*
 * Three adjacent constant-String appends merge into a single append,
 * eliminating two of them.
 */
TEST_F(StringBuilderAppendChainTest, threeConstantAppendsMergeIntoOne) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "c")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  EXPECT_EQ(run_merge(method), 2);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (const-string "abc")
      (move-result-pseudo-object v2)
      (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "b")
      (move-result-pseudo-object v1)
      (const-string "c")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

/*
 * One builder reached through two registers: the appends on each register merge
 * on their own, so the append that starts the second group is not skipped over
 * along with the first group.
 */
TEST_F(StringBuilderAppendChainTest,
       constantAppendsInTwoRegistersMergeSeparately) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (move-object v2 v0)
        (const-string "c")
        (move-result-pseudo-object v1)
        (invoke-virtual (v2 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v2)
        (const-string "d")
        (move-result-pseudo-object v1)
        (invoke-virtual (v2 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v2)
        (invoke-virtual (v2) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v3)
        (return-object v3)
      )
    )
  )");

  EXPECT_EQ(run_merge(method), 2);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "a")
      (move-result-pseudo-object v1)
      (const-string "ab")
      (move-result-pseudo-object v4)
      (invoke-virtual (v0 v4) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "b")
      (move-result-pseudo-object v1)
      (move-object v2 v0)
      (const-string "c")
      (move-result-pseudo-object v1)
      (const-string "cd")
      (move-result-pseudo-object v5)
      (invoke-virtual (v2 v5) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v2)
      (const-string "d")
      (move-result-pseudo-object v1)
      (invoke-virtual (v2) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v3)
      (return-object v3)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

/*
 * A non-constant append between two constant appends breaks adjacency: neither
 * constant has a constant neighbor, so nothing merges.
 */
TEST_F(StringBuilderAppendChainTest,
       constantAppendsSeparatedByNonConstantNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v3)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * A lone constant append (its only neighbor is non-constant) has no adjacent
 * constant to merge with, so the builder is left alone.
 */
TEST_F(StringBuilderAppendChainTest, singleConstantAppendNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v3)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * When an append's move-result writes a register other than the builder (so
 * the chain does not thread a single register) and a later consumer reads that
 * register, merging would leave the consumer's register undefined. Such
 * appends are left alone.
 */
TEST_F(StringBuilderAppendChainTest, nonSelfLoopChainNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v2)
        (invoke-virtual (v2) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * A builder moved into another builder's register threads a different register
 * than its earlier appends did, which separates the two so they cannot merge.
 */
TEST_F(StringBuilderAppendChainTest, buildersSharingARegisterNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "a")
        (move-result-pseudo-object v1)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v2)
        (invoke-direct (v2) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "b")
        (move-result-pseudo-object v3)
        (invoke-virtual (v2 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v2)
        (move-object v0 v2)
        (const-string "c")
        (move-result-pseudo-object v4)
        (invoke-virtual (v0 v4) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * Each append's move-result sits in a successor block rather than next to it,
 * which the try region forces. The appends still merge, and the dropped one's
 * move-result is removed along with it.
 */
TEST_F(StringBuilderAppendChainTest, crossBlockMoveResultConstantAppendsMerge) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v5)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v5) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "b")
        (move-result-pseudo-object v1)
        (.try_start t)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "c")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (.try_end t)
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
        (.catch (t))
        (move-exception v0)
        (throw v0)
      )
    )
  )");

  EXPECT_EQ(run_merge(method), 1);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v5)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (invoke-virtual (v0 v5) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "b")
      (move-result-pseudo-object v1)
      (.try_start t)
      (const-string "bc")
      (move-result-pseudo-object v6)
      (invoke-virtual (v0 v6) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (const-string "c")
      (move-result-pseudo-object v2)
      (.try_end t)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
      (.catch (t))
      (move-exception v0)
      (throw v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

/*
 * Each append's move-result sits in a successor block, which the try region
 * forces. The first of two constant appends writes a register other than the
 * builder, so it is not a self loop and the two are left alone.
 */
TEST_F(StringBuilderAppendChainTest,
       crossBlockMoveResultFirstAppendInOtherRegisterNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "b")
        (move-result-pseudo-object v1)
        (.try_start t)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v3)
        (const-string "c")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (.try_end t)
        (invoke-virtual (v3) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
        (.catch (t))
        (move-exception v0)
        (throw v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * Each append's move-result sits in a successor block, which the try region
 * forces. The second of two constant appends writes a register other than the
 * builder and the toString() reads it, so merging would drop that append and
 * delete the register's only definition.
 */
TEST_F(StringBuilderAppendChainTest,
       crossBlockMoveResultDroppedAppendInOtherRegisterNotMerged) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:()Ljava/lang/String;"
      (
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (const-string "b")
        (move-result-pseudo-object v1)
        (.try_start t)
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v0)
        (const-string "c")
        (move-result-pseudo-object v2)
        (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (move-result-object v3)
        (.try_end t)
        (invoke-virtual (v3) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
        (.catch (t))
        (move-exception v0)
        (throw v0)
      )
    )
  )");

  auto original = assembler::to_s_expr(method->get_code());
  EXPECT_EQ(run_merge(method), 0);
  EXPECT_EQ(original, assembler::to_s_expr(method->get_code()));
}

/*
 * Both reductions together: the merge shortens a three-append chain to the two
 * appends `reduce_two_append_concats` needs, which then rewrites the
 * toString().
 */
TEST_F(StringBuilderAppendChainTest, mergedChainIsReducedToConcat) {
  auto* method = assembler::method_from_string(R"(
    (method (public static) "LTest;.f:(Ljava/lang/String;)Ljava/lang/String;"
      (
        (load-param-object v1)
        (invoke-virtual (v1) "Ljava/lang/String;.length:()I")
        (move-result v2)
        (new-instance "Ljava/lang/StringBuilder;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
        (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "a")
        (move-result-pseudo-object v3)
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (const-string "b")
        (move-result-pseudo-object v3)
        (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
        (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
        (move-result-object v0)
        (return-object v0)
      )
    )
  )");

  auto [merged, reduced] = run_merge_then_concat(method);
  EXPECT_EQ(merged, 1);
  EXPECT_EQ(reduced, 1);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (invoke-virtual (v1) "Ljava/lang/String;.length:()I")
      (move-result v2)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (move-object v5 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "a")
      (move-result-pseudo-object v3)
      (const-string "ab")
      (move-result-pseudo-object v4)
      (move-object v6 v4)
      (invoke-virtual (v0 v4) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "b")
      (move-result-pseudo-object v3)
      (invoke-virtual (v5 v6) "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}
