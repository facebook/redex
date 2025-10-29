/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelChecker.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Inliner.h"
#include "RedexTest.h"
#include "Shrinker.h"
#include "TailDuplicationPass.h"
#include "Walkers.h"

struct TailDuplicationTest : public RedexTest {
  TailDuplicationTest() {
    DexMethod::make_method("Ljava/lang/Object;.<init>:()V")
        ->make_concrete(ACC_CONSTRUCTOR | ACC_PUBLIC, false);
    DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z")
        ->make_concrete(ACC_PUBLIC, true);

    DexField::make_field("Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    DexField::make_field("Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);

    DexMethod::make_method("Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC, true);
    DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z")
        ->make_concrete(ACC_PUBLIC, true);
  }
};

size_t make_hot_tails_unique(DexMethod* method, bool shrink = false) {
  auto& code = *method->get_code();
  code.build_cfg();
  size_t new_blocks = tail_duplication_impl::make_hot_tails_unique(code.cfg());

  if (shrink) {
    ClassCreator class_creator(method->get_class());
    class_creator.set_super(type::java_lang_Object());
    class_creator.add_method(method);
    auto* cls = class_creator.create();

    auto store = DexStore("store");
    store.add_classes({cls});
    DexStoresVector stores{store};
    auto scope = build_class_scope(stores);
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);

    ConfigFiles conf = ConfigFiles(Json::nullValue);
    using namespace shrinker;
    ShrinkerConfig shrinker_config;
    shrinker_config.run_const_prop = true;
    shrinker_config.run_copy_prop = true;
    shrinker_config.run_local_dce = true;
    shrinker_config.run_dedup_blocks = true;
    shrinker_config.compute_pure_methods = false;
    int min_sdk = 0;
    Shrinker shrinker(stores,
                      scope,
                      init_classes_with_side_effects,
                      conf,
                      shrinker_config,
                      min_sdk);

    shrinker.shrink_method(method);
  }

  code.clear_cfg();
  return new_blocks;
}

TEST_F(TailDuplicationTest, nothing) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:()V"
      (
        (return-void)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 0);

  auto expected_code = assembler::ircode_from_string(R"(
  (
      (return-void)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, basic) {
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (goto :common)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, basic_shrink_undo_hot_hot) {
  // When there is nothing to specialize, shrinking will effectively undo the
  // duplication.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (goto :common)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method, /* shrink */ true);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (goto :common)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, basic_shrink_undo_hot_cold) {
  // When there is nothing to specialize, shrinking will effectively undo the
  // duplication.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (0.0 0.0))
        (const v0 1)
        (goto :common)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method, /* shrink */ true);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (0.0 0.0))
        (const v0 1)
        (goto :common)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, specialize) {
  // Specialization "survives" shrinking.

  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (goto :common)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (add-int/lit v0 v0 1)
        (return v0)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method, /* shrink */ true);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 4294967295 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 2)
        (goto :common)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, specialize2) {
  // Specialization "survives" shrinking, but can also lead to code size
  // increase.

  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 1)
        (goto :common)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (invoke-static () "LOverhead;.sticky1:()V")
        (invoke-static () "LOverhead;.sticky2:()V")
        (add-int/lit v0 v0 1)
        (return v0)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method, /* shrink */ true);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (invoke-static () "LOverhead;.sticky1:()V")
        (invoke-static () "LOverhead;.sticky2:()V")
        (const v0 1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 4294967295 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (invoke-static () "LOverhead;.sticky1:()V")
        (invoke-static () "LOverhead;.sticky2:()V")
        (const v0 2)
        (goto :common)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, hot_only_rewrite_cold_info) {
  // We can even rewrite the source block hotness of the remaining block to be
  // cold, as we duplicated all the hot instances.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (switch v0 (:hot :cold0 :cold1))
        (.src_block "LTail;.duplication:(I)V" 0 (0.0 0.0))
        (const v0 -1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (return v0)

        (:hot 0)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:cold0 1)
        (.src_block "LTail;.duplication:(I)V" 3 (0.0 0.0))
        (const v0 0)
        (goto :common)

        (:cold1 2)
        (.src_block "LTail;.duplication:(I)V" 4 (0.0 0.0))
        (const v0 1)
        (goto :common)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (switch v0 (:hot :cold0 :cold1))
        (.src_block "LTail;.duplication:(I)V" 0 (0.0 0.0))
        (const v0 -1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 1 (0.0 0.0))
        (return v0)

        (:cold1 2)
        (.src_block "LTail;.duplication:(I)V" 4 (0.0 0.0))
        (const v0 1)
        (goto :common)

        (:cold0 1)
        (.src_block "LTail;.duplication:(I)V" 3 (0.0 0.0))
        (const v0 0)
        (goto :common)

        (:hot 0)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (const v0 0)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (return v0)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, hot_only_missing_cold_info) {
  // Without source blocks in all pred blocks, we cannot properly rewrite the
  // source block hotness of the remaining block.

  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (switch v0 (:hot :cold0 :cold1))
        (const v0 -1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (return v0)

        (:hot 0)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 0)
        (goto :common)

        (:cold0 1)
        (const v0 0)
        (goto :common)

        (:cold1 2)
        (const v0 1)
        (goto :common)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 1);

  auto expected_code = assembler::ircode_from_string(R"(
  (
        (load-param v0)
        (switch v0 (:hot :cold0 :cold1))
        (const v0 -1)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (return v0)

        (:cold1 2)
        (const v0 1)
        (goto :common)

        (:cold0 1)
        (const v0 0)
        (goto :common)

        (:hot 0)
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 0)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (return v0)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, loop) {
  // Don't duplicate the loop header (or any block with back-edges).

  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:()I"
      (
        (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
        (const v0 10)

        (:loop)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (if-nez v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 3 (1.0 1.0))
        (return v0)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 4 (1.0 1.0))
        (add-int/lit v0 v0 -1)
        (goto :loop)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 0);

  auto expected_code = assembler::ircode_from_string(R"(
  (
    (.src_block "LTail;.duplication:(I)V" 1 (1.0 1.0))
    (const v0 10)

    (:loop)
    (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
    (if-nez v0 :true)

    (:false)
    (.src_block "LTail;.duplication:(I)V" 3 (1.0 1.0))
    (return v0)

    (:true)
    (.src_block "LTail;.duplication:(I)V" 4 (1.0 1.0))
    (add-int/lit v0 v0 -1)
    (goto :loop)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(TailDuplicationTest, no_explosion) {
  // Only duplicate a single hot path.
  auto* method = assembler::method_from_string(std::string("") + R"(
    (method (public static) "LTail;.duplication:(I)I"
      (
        (load-param v0)
        (.src_block "LTail;.duplication:(I)V" 0 (1.0 1.0))
        (if-eqz v0 :true)

        (:false)
        (.src_block "LTail;.duplication:(I)V" 1 (0.0 0.0))
        (const v0 0)

        (:common)
        (.src_block "LTail;.duplication:(I)V" 2 (1.0 1.0))
        (if-eqz v0 :true2)

        (:false2)
        (.src_block "LTail;.duplication:(I)V" 4 (0.0 0.0))
        (add-int/lit v0 v0 2)

        (:common2)
        (.src_block "LTail;.duplication:(I)V" 5 (1.0 1.0))
        (return v0)

        (:true2)
        (.src_block "LTail;.duplication:(I)V" 6 (1.0 1.0))
        (add-int/lit v0 v0 3)
        (goto :common2)

        (:true)
        (.src_block "LTail;.duplication:(I)V" 3 (1.0 1.0))
        (const v0 1)
        (goto :common)
      )
    )
  )");

  size_t new_blocks = make_hot_tails_unique(method);

  ASSERT_EQ(new_blocks, 3);

  auto expected_code = assembler::ircode_from_string(R"(
  (
    (load-param v0)
    (.src_block "LTail;.duplication:(I)V" 0 (1.000000 1.000000))
    (if-eqz v0 :true)

    (.src_block "LTail;.duplication:(I)V" 1 (0.000000 0.000000))
    (const v0 0)
    (.src_block "LTail;.duplication:(I)V" 2 (0.000000 0.000000))
    (if-eqz v0 :true2)

    (:false2)
    (.src_block "LTail;.duplication:(I)V" 4 (0.000000 0.000000))
    (add-int/lit v0 v0 2)

    (:common2)
    (.src_block "LTail;.duplication:(I)V" 5 (0.000000 0.000000))
    (return v0)

    (:true2)
    (.src_block "LTail;.duplication:(I)V" 6 (0.000000 0.000000))
    (add-int/lit v0 v0 3)
    (goto :common2)

    (:true)
    (.src_block "LTail;.duplication:(I)V" 3 (1.000000 1.000000))
    (const v0 1)
    (.src_block "LTail;.duplication:(I)V" 2 (1.000000 1.000000))
    (if-eqz v0 :true3)
    (goto :false2)

    (:true3)
    (.src_block "LTail;.duplication:(I)V" 6 (1.000000 1.000000))
    (add-int/lit v0 v0 3)
    (.src_block "LTail;.duplication:(I)V" 5 (1.000000 1.000000))
    (return v0)
  ))");
  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}
