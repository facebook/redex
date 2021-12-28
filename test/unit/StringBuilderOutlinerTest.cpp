/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringBuilderOutliner.h"

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "JarLoader.h"
#include "ObjectSensitiveDcePass.h"
#include "RedexTest.h"

using namespace stringbuilder_outliner;

namespace ptrs = local_pointers;
namespace uv = used_vars;

class StringBuilderOutlinerTest : public RedexTest {
 public:
  void SetUp() override {
    const char* android_sdk = std::getenv("sdk_path");
    std::string android_target(std::getenv("android_target"));
    std::string sdk_jar = std::string(android_sdk) + "/platforms/" +
                          android_target + "/android.jar";
    // StringBuilderOutliner requires bunch of java.lang.* classes to be
    // defined. Loading the SDK JAR here ensures that.
    ASSERT_TRUE(load_jar_file(sdk_jar.c_str()));

    m_config.min_outline_count = 1;

    m_stores.emplace_back("classes");
    m_stores[0].get_dexen().emplace_back();

    auto init_method =
        DexMethod::get_method("Ljava/lang/StringBuilder;.<init>:()V");
    ASSERT_NE(init_method, nullptr);
    m_escape_summary_map.emplace(init_method, ptrs::EscapeSummary{});
    m_effect_summary_map.emplace(init_method, side_effects::Summary({0}));

    auto init_string_method = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V");
    ASSERT_NE(init_string_method, nullptr);
    m_escape_summary_map.emplace(init_string_method, ptrs::EscapeSummary{});
    m_effect_summary_map.emplace(init_string_method,
                                 side_effects::Summary({0}));

    auto append_string_method = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
        "StringBuilder;");
    ASSERT_NE(append_string_method, nullptr);
    m_escape_summary_map.emplace(append_string_method,
                                 ptrs::EscapeSummary(ptrs::ParamSet{0}, {}));
    m_effect_summary_map.emplace(append_string_method,
                                 side_effects::Summary({0}));

    auto append_long_method = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.append:(J)Ljava/lang/StringBuilder;");
    ASSERT_NE(append_long_method, nullptr);
    m_escape_summary_map.emplace(append_long_method,
                                 ptrs::EscapeSummary(ptrs::ParamSet{0}, {}));
    m_effect_summary_map.emplace(append_long_method,
                                 side_effects::Summary({0}));

    auto tostring_method = DexMethod::get_method(
        "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;");
    ASSERT_NE(tostring_method, nullptr);
    m_escape_summary_map.emplace(
        tostring_method,
        ptrs::EscapeSummary(ptrs::ParamSet{ptrs::FRESH_RETURN}, {}));
    m_effect_summary_map.emplace(tostring_method, side_effects::Summary());
  }

 protected:
  void run_outliner(IRCode* code) {
    Outliner outliner(m_config);
    outliner.analyze(*code);
    outliner.create_outline_helpers(&m_stores);
    outliner.transform(code);

    // Use OSDCE to remove any unused new-instance StringBuilder opcodes. When
    // running this pass against an app, the app's redex config should always
    // contain a run of OSDCE after StringBuilderOutlinerPass.
    remove_dead_instructions(code);
  }

  void populate_summary_maps(
      const IRCode& code,
      ptrs::InvokeToSummaryMap* invoke_to_esc_summary_map,
      side_effects::InvokeToSummaryMap* invoke_to_eff_summary_map) {
    const auto& cfg = code.cfg();
    for (const auto* block : cfg.blocks()) {
      for (const auto& mie : InstructionIterable(block)) {
        const auto* insn = mie.insn;
        if (!opcode::is_an_invoke(insn->opcode())) {
          continue;
        }
        auto method = insn->get_method();
        if (!m_escape_summary_map.count(method)) {
          continue;
        }
        invoke_to_esc_summary_map->emplace(insn,
                                           m_escape_summary_map.at(method));
        invoke_to_eff_summary_map->emplace(insn,
                                           m_effect_summary_map.at(method));
      }
    }
  }

  void remove_dead_instructions(IRCode* code) {
    auto& cfg = code->cfg();
    ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
    side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
    populate_summary_maps(*code, &invoke_to_esc_summary_map,
                          &invoke_to_eff_summary_map);

    ptrs::FixpointIterator fp_iter(cfg, invoke_to_esc_summary_map);
    fp_iter.run(ptrs::Environment());
    uv::FixpointIterator used_vars_fp_iter(fp_iter, invoke_to_eff_summary_map,
                                           cfg);
    used_vars_fp_iter.run(uv::UsedVarsSet());

    for (const auto& it : uv::get_dead_instructions(*code, used_vars_fp_iter)) {
      code->remove_opcode(it);
    }
  }

  Config m_config;
  DexStoresVector m_stores;

  std::unordered_map<const DexMethodRef*, ptrs::EscapeSummary>
      m_escape_summary_map;
  std::unordered_map<const DexMethodRef*, side_effects::Summary>
      m_effect_summary_map;
};

TEST_F(StringBuilderOutlinerTest, outlineTwo) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (invoke-static (v2 v3) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * Check that we handle the StringBuilder(String) constructor correctly.
 */
TEST_F(StringBuilderOutlinerTest, stringArgBuilderConstructor) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v1) "Ljava/lang/StringBuilder;.<init>:(Ljava/lang/String;)V")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (invoke-static (v2 v3) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

TEST_F(StringBuilderOutlinerTest, trackReturnedStringBuilders) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0) ; make sure we track StringBuilder instances as they get returned
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (move-result-object v0)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (invoke-static (v2 v3) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

TEST_F(StringBuilderOutlinerTest, outlineThree) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (move-object v4 v1)
      (invoke-static (v2 v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());

  auto outline_cls =
      type_class(DexType::get_type("Lcom/redex/OutlinedStringBuilders;"));
  ASSERT_EQ(outline_cls->get_dmethods().size(), 1);
  auto outline_helper_method = outline_cls->get_dmethods().at(0);
  auto expected_outlined_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v1)
      (load-param-object v2)
      (load-param-object v3)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v2) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v3) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  // Check that OSDCE recognizes the outline helper as side-effect-free. This
  // ensures that running StringBuilderOutlinerPass before OSDCE won't
  // inadvertently cause dead code to be retained.
  auto outline_helper_code = outline_helper_method->get_code();
  EXPECT_CODE_EQ(expected_outlined_code.get(), outline_helper_code);

  outline_helper_code->build_cfg(false);
  auto& outline_helper_cfg = outline_helper_code->cfg();
  outline_helper_cfg.calculate_exit_block();

  ptrs::InvokeToSummaryMap invoke_to_esc_summary_map;
  side_effects::InvokeToSummaryMap invoke_to_eff_summary_map;
  populate_summary_maps(*outline_helper_code,
                        &invoke_to_esc_summary_map,
                        &invoke_to_eff_summary_map);

  ptrs::FixpointIterator ptrs_fp_iter(outline_helper_cfg,
                                      invoke_to_esc_summary_map);
  ptrs_fp_iter.run(ptrs::Environment());
  auto esc_summary =
      ptrs::get_escape_summary(ptrs_fp_iter, *outline_helper_code);
  EXPECT_EQ(esc_summary.returned_parameters,
            ptrs::ParamSet{ptrs::FRESH_RETURN});
  EXPECT_EQ(esc_summary.escaping_parameters.size(), 0);

  uv::FixpointIterator used_vars_fp_iter(
      ptrs_fp_iter, invoke_to_eff_summary_map, outline_helper_cfg);
  used_vars_fp_iter.run(uv::UsedVarsSet());

  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      {}, /* create_init_class_insns */ false);
  auto eff_summary = side_effects::analyze_code(
      init_classes_with_side_effects, invoke_to_eff_summary_map, ptrs_fp_iter,
      outline_helper_code);
  EXPECT_EQ(eff_summary, side_effects::Summary(side_effects::EFF_NONE, {}));
}

TEST_F(StringBuilderOutlinerTest, outlineWide) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-wide v1 123)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(J)Ljava/lang/StringBuilder;")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-wide v1 123)
      (move-wide v2 v1)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v4 v1)
      (invoke-static (v2 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(JLjava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

TEST_F(StringBuilderOutlinerTest, builderUsedInBranches) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo") ; this value is reused by the two toString() calls
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (if-eqz v2 :true-label)
      (const-string "bar")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)

      (:done)
      (return-object v0)

      (:true-label)
      (const-string "baz")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (goto :done)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v3 v1)

      (if-eqz v2 :true-label)
      (const-string "bar")
      (move-result-pseudo-object v1)
      (move-object v4 v1)
      (invoke-static (v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)

      (:done)
      (return-object v0)

      (:true-label)
      (const-string "baz")
      (move-result-pseudo-object v1)
      (move-object v5 v1)
      (invoke-static (v3 v5) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (goto :done)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * Check that we do not try to outline a StringBuilder if it is being appended
 * to in a loop.
 */
TEST_F(StringBuilderOutlinerTest, builderUsedinLoop) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (:loop)
      (if-eqz v2 :end-loop)
      (const-string "bar")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (add-int/lit8 v2 v2 1)
      (goto :loop)
      (:end-loop)

      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  auto expected = assembler::to_s_expr(code.get());
  run_outliner(code.get());
  EXPECT_EQ(expected, assembler::to_s_expr(code.get()));
}

/*
 * Check that do outline a StringBuilder even in the presence of a loop, as long
 * as that loop does not mutate it.
 */
TEST_F(StringBuilderOutlinerTest, builderNotUsedinLoop) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (const-string "bar")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      ; This loop does not mutate the StringBuilder, so the toString() call
      ; below can still be outlined.
      (:loop)
      (if-eqz v2 :end-loop)
      (add-int/lit8 v2 v2 1)
      (goto :loop)
      (:end-loop)

      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param v2)
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v3 v1)
      (const-string "bar")
      (move-result-pseudo-object v1)
      (move-object v4 v1)

      (:loop)
      (if-eqz v2 :end-loop)
      (add-int/lit8 v2 v2 1)
      (goto :loop)
      (:end-loop)

      (invoke-static (v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * Check that we do not try to outline a StringBuilder if it is being passed a
 * mutable value.
 */
TEST_F(StringBuilderOutlinerTest, mutableCharSequence) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")

      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "Ljava/lang/StringBuilder;.<init>:()V")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;")

      (invoke-static (v1) "Lcom/test/Foo;.unknownMutation:(Ljava/lang/StringBuilder;)V")

      ; If we replaced this toString() call with an outlined helper method
      ; taking the string "foo" and the StringBuilder instance in v1, we would
      ; get incorrect results at runtime. The StringBuilder in v0 is reading the
      ; contents of the StringBuilder in v1 before the unknownMutation() call,
      ; but those contents may have changed by the time our outline helper
      ; method is called.
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  auto expected = assembler::to_s_expr(code.get());
  run_outliner(code.get());
  EXPECT_EQ(expected, assembler::to_s_expr(code.get()));
}

/*
 * Check that we do not create invalid code if the builder is live-out. We will
 * will still outline the code but we won't remove the append instructions. This
 * does indeed cause code bloat instead of code reduction, but it's a pretty
 * rare case.
 */
TEST_F(StringBuilderOutlinerTest, builderAliasIsLiveOut) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (move-object v2 v0) ; create alias

      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (const-string "baz")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)

      ; v2 is live-out after the toString() call
      (invoke-static (v2) "Lcom/redex/Unknown;.foo:(Ljava/lang/StringBuilder;)V")

      (return-object v0)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (move-object v2 v0)

      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v3 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (const-string "baz")
      (move-result-pseudo-object v1)
      (move-object v4 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (invoke-static (v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)

      (invoke-static (v2) "Lcom/redex/Unknown;.foo:(Ljava/lang/StringBuilder;)V")
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * We don't handle multiple toString() calls on the same StringBuilder
 * efficiently, but that's a rare case anyway. However, this unit test at least
 * ensures that we don't generate invalid code.
 */
TEST_F(StringBuilderOutlinerTest, multipleToStringCalls) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")

      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (const-string "baz")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v1)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v2)

      (invoke-static (v1 v2) "Lcom/redex/Unknown;.foo:(Ljava/lang/String;Ljava/lang/String;)V")
      (return-void)
    )
  )");

  run_outliner(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")

      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v3 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (const-string "baz")
      (move-result-pseudo-object v1)
      (move-object v4 v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")

      (invoke-static (v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v1)
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v2)

      (invoke-static (v1 v2) "Lcom/redex/Unknown;.foo:(Ljava/lang/String;Ljava/lang/String;)V")
      (return-void)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * Check that the min_outline_count config setting is respected.
 */
TEST_F(StringBuilderOutlinerTest, minCount) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  auto original = assembler::to_s_expr(code.get());
  m_config.min_outline_count = 2;
  run_outliner(code.get());
  EXPECT_EQ(original, assembler::to_s_expr(code.get()));

  m_config.min_outline_count = 1;
  run_outliner(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (move-object v4 v1)
      (invoke-static (v2 v3 v4) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}

/*
 * Check that the max_outline_length config setting is respected.
 */
TEST_F(StringBuilderOutlinerTest, maxLength) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/StringBuilder;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/StringBuilder;.<init>:()V")
      (const-string "foo")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0 v1) "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/StringBuilder;")
      (invoke-virtual (v0) "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");

  auto original = assembler::to_s_expr(code.get());
  m_config.max_outline_length = 3;
  run_outliner(code.get());
  EXPECT_EQ(original, assembler::to_s_expr(code.get()));

  m_config.max_outline_length = 4;
  run_outliner(code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "foo")
      (move-result-pseudo-object v1)
      (move-object v2 v1)
      (move-object v3 v1)
      (move-object v4 v1)
      (move-object v5 v1)
      (invoke-static (v2 v3 v4 v5) "Lcom/redex/OutlinedStringBuilders;.concat:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;")
      (move-result-object v0)
      (return-object v0)
    )
  )");
  EXPECT_CODE_EQ(expected_code.get(), code.get());
}
