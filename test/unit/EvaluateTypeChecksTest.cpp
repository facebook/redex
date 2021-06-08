/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EvaluateTypeChecks.h"

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "TypeUtil.h"

using namespace type;

class EvaluateTypeChecksTest : public RedexTest {
 public:
  void SetUp() override {
    auto simple_class = [](const std::string& name, const DexType* super_type) {
      ClassCreator cc(DexType::make_type(name.c_str()));
      cc.set_super(const_cast<DexType*>(super_type));
      return cc.create()->get_type();
    };
    m_foo = simple_class("LFoo;", java_lang_Throwable());
    m_bar = simple_class("LBar;", m_foo);
    m_baz = simple_class("LBaz;", m_foo);
  }

 protected:
  // Wrapper to unpack optional. Returns -1 for none.
  static int32_t evaluate(const DexType* src, const DexType* test) {
    auto res = check_casts::EvaluateTypeChecksPass::evaluate(src, test);
    if (res) {
      return *res;
    }
    return -1;
  }

  static std::string regularize(const std::string& s) {
    auto code = assembler::ircode_from_string(s);
    return assembler::to_string(code.get());
  }

  ::testing::AssertionResult run(const std::string& type,
                                 const std::string& method_line,
                                 const std::string& in,
                                 const std::string& out) {
    auto store = DexStore("store");
    store.add_classes(
        {type_class(m_foo), type_class(m_bar), type_class(m_baz)});
    DexStoresVector stores{store};
    auto scope = build_class_scope(stores);
    XStoreRefs xstores(stores);

    auto method_str = std::string("(") + method_line + " " + in + " )";
    auto method = assembler::class_with_method(type, method_str);

    check_casts::EvaluateTypeChecksPass::optimize(method, xstores);

    auto expected_str = regularize(out);
    auto actual_str = assembler::to_string(method->get_code());
    if (expected_str == actual_str) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "Expected:\n"
                                         << expected_str << "\nActual:\n"
                                         << actual_str;
  }

  // Will be created in SetUp. Hierarchy is:
  //
  // Object -> Throwable -> foo -> bar
  //                            -> baz
  //
  // (Using Throwable for shortcut)

  DexType* m_foo = nullptr;
  DexType* m_bar = nullptr;
  DexType* m_baz = nullptr;
};

TEST_F(EvaluateTypeChecksTest, same_type) {
  auto obj = java_lang_Object();
  EXPECT_EQ(1, evaluate(obj, obj));

  auto str = java_lang_String();
  EXPECT_EQ(1, evaluate(str, str));

  EXPECT_EQ(1, evaluate(m_foo, m_foo));
  EXPECT_EQ(1, evaluate(m_bar, m_bar));
  EXPECT_EQ(1, evaluate(m_baz, m_baz));
}

TEST_F(EvaluateTypeChecksTest, external_external) {
  auto obj = java_lang_Object();
  auto str = java_lang_String();
  auto cls = java_lang_Class();

  // Object is special.
  EXPECT_EQ(1, evaluate(str, obj));

  // For now, we expect this to not be resolved.
  EXPECT_EQ(-1, evaluate(obj, str));
  EXPECT_EQ(-1, evaluate(str, cls));
  EXPECT_EQ(-1, evaluate(cls, str));
}

TEST_F(EvaluateTypeChecksTest, external_internal) {
  auto obj = java_lang_Object();
  auto str = java_lang_String();

  // Object is special.
  EXPECT_EQ(1, evaluate(m_foo, obj));

  // For now, we expect this to not be resolved.
  EXPECT_EQ(-1, evaluate(obj, m_foo));
  EXPECT_EQ(-1, evaluate(str, m_foo));
  EXPECT_EQ(-1, evaluate(m_foo, str));
}

TEST_F(EvaluateTypeChecksTest, internal_yes) {
  EXPECT_EQ(1, evaluate(m_foo, m_foo));
  EXPECT_EQ(1, evaluate(m_bar, m_bar));
  EXPECT_EQ(1, evaluate(m_baz, m_baz));

  EXPECT_EQ(1, evaluate(m_bar, m_foo));
  EXPECT_EQ(1, evaluate(m_baz, m_foo));
}

TEST_F(EvaluateTypeChecksTest, internal_no) {
  EXPECT_EQ(0, evaluate(m_bar, m_baz));
  EXPECT_EQ(0, evaluate(m_baz, m_bar));
}

TEST_F(EvaluateTypeChecksTest, internal_not_static) {
  EXPECT_EQ(-1, evaluate(m_foo, m_bar));
  EXPECT_EQ(-1, evaluate(m_foo, m_baz));
}

// Full optimization tests.

TEST_F(EvaluateTypeChecksTest, instance_of_no_optimize) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LBar;")
        (move-result-pseudo v0)

        (if-nez v0 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LFoo;)I\"";

  EXPECT_TRUE(run("LTest;", method_str, code, code));
}

TEST_F(EvaluateTypeChecksTest, instance_of_optimize_always_fail) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LBar;")
        (move-result-pseudo v0)

        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = "((load-param-object v0) (const v0 0) (return v0))";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}

TEST_F(EvaluateTypeChecksTest, instance_of_optimize_always_succeed_nez) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LFoo;")
        (move-result-pseudo v0)

        (if-nez v0 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = R"(
      (
       (load-param-object v0)
       (move-object v1 v0)
       (if-nez v1 :L0)
       (const v0 0)
       (return v0)
       (:L0)
       (const v0 1)
       (return v0)
      )
     )";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}

TEST_F(EvaluateTypeChecksTest, instance_of_optimize_always_succeed_eqz) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LFoo;")
        (move-result-pseudo v0)

        (if-eqz v0 :L1)
        (const v0 1)
        (return v0)

        (:L1)
        (const v0 0)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = R"(
      (
       (load-param-object v0)
       (move-object v1 v0)
       (if-eqz v1 :L0)
       (const v0 1)
       (return v0)
       (:L0)
       (const v0 0)
       (return v0)
      )
     )";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}

TEST_F(EvaluateTypeChecksTest, instance_of_optimize_always_succeed_nez_chain) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LFoo;")
        (move-result-pseudo v1)

        (move v2 v1)

        (if-nez v2 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = R"(
      (
       (load-param-object v0)
       (if-nez v0 :L0)
       (const v0 0)
       (return v0)
       (:L0)
       (const v0 1)
       (return v0)
      )
     )";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}

TEST_F(EvaluateTypeChecksTest,
       instance_of_no_optimize_always_succeed_nez_multi_use) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LFoo;")
        (move-result-pseudo v0)

        (move v1 v0)
        (move v2 v0)

        (if-nez v1 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  EXPECT_TRUE(run("LTest;", method_str, code, code));
}

TEST_F(EvaluateTypeChecksTest,
       instance_of_no_optimize_always_succeed_nez_no_branch) {
  auto code = R"(
       (
        (load-param-object v0)
        (instance-of v0 "LFoo;")
        (move-result-pseudo v0)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  EXPECT_TRUE(run("LTest;", method_str, code, code));
}

TEST_F(EvaluateTypeChecksTest, instance_of_multi_def) {
  auto code = R"(
       (
        (load-param v0)
        (load-param v1)
        (load-param-object v2)

        (if-eqz v0 :L1)

        (instance-of v2 "LBar;")
        (move-result-pseudo v1)

        (:L1)
        (if-nez v1 :L2)
        (const v0 0)
        (return v0)

        (:L2)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(ZILBar;)I\"";

  EXPECT_TRUE(run("LTest;", method_str, code, code));
}

TEST_F(EvaluateTypeChecksTest, check_cast_no_optimize) {
  auto code = R"(
       (
        (load-param-object v0)
        (check-cast v0 "LBar;")
        (move-result-pseudo v0)

        (if-nez v0 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LFoo;)I\"";

  EXPECT_TRUE(run("LTest;", method_str, code, code));
}

TEST_F(EvaluateTypeChecksTest, check_cast_optimize_always_fail) {
  auto code = R"(
       (
        (load-param-object v0)
        (check-cast v0 "LBar;")
        (move-result-pseudo v0)

        (if-nez v0 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = "((load-param-object v0) (const v0 0) (return v0))";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}

TEST_F(EvaluateTypeChecksTest, check_cast_optimize_always_succeed) {
  auto code = R"(
       (
        (load-param-object v0)
        (check-cast v0 "LFoo;")
        (move-result-pseudo v0)

        (if-nez v0 :L1)
        (const v0 0)
        (return v0)

        (:L1)
        (const v0 1)
        (return v0)
       )
      )";
  auto method_str = "method (private static) \"LTest;.test:(LBaz;)I\"";

  auto expected = R"(
      (
       (load-param-object v0)
       (if-nez v0 :L0)
       (const v0 0)
       (return v0)
       (:L0)
       (const v0 1)
       (return v0)
      )
     )";
  EXPECT_TRUE(run("LTest;", method_str, code, expected));
}
