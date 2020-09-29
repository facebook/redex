/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "FinalInlineV2.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

struct FinalInlineTest : public RedexTest {
 public:
  void SetUp() override {
    m_cc = std::make_unique<ClassCreator>(DexType::make_type("LFoo;"));
    m_cc->set_super(type::java_lang_Object());
  }

 protected:
  DexField* create_field_with_zero_value(const char* name) {
    auto field = static_cast<DexField*>(DexField::make_field(name));
    auto encoded_value = DexEncodedValue::zero_for_type(field->get_type());
    field->make_concrete(ACC_PUBLIC | ACC_STATIC, encoded_value);
    m_cc->add_field(field);
    return field;
  }

  std::unique_ptr<ClassCreator> m_cc;
};

TEST_F(FinalInlineTest, encodeValues) {
  auto field = create_field_with_zero_value("LFoo;.bar:I");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  FinalInlinePassV2::run({cls}, /* xstores */ nullptr);

  EXPECT_EQ(cls->get_clinit(), nullptr);
  EXPECT_EQ(field->get_static_value()->value(), 1);
}

TEST_F(FinalInlineTest, encodeTypeValues) {
  ClassCreator cc2(DexType::make_type("LBar;"));
  cc2.set_super(type::java_lang_Object());
  auto cls2 = cc2.create();

  auto field = create_field_with_zero_value("LFoo;.bar:Ljava/lang/Class;");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const-class "LBar;")
      (move-result-pseudo-object v0)
      (sput v0 "LFoo;.bar:Ljava/lang/Class;")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto store = DexStore("store");
  store.add_classes({cls, cls2});
  DexStoresVector stores{store};
  auto scope = build_class_scope(stores);
  XStoreRefs xstores(stores);
  FinalInlinePassV2::run(scope, &xstores);

  EXPECT_EQ(cls->get_clinit(), nullptr);
  EXPECT_EQ(field->get_static_value()->evtype(), DEVT_TYPE);
  EXPECT_EQ(
      static_cast<DexEncodedValueType*>(field->get_static_value())->type(),
      cls2->get_type());
}

TEST_F(FinalInlineTest, encodeTypeValuesXStore) {
  ClassCreator cc2(DexType::make_type("LBar;"));
  cc2.set_super(type::java_lang_Object());
  auto cls2 = cc2.create();

  auto field = create_field_with_zero_value("LFoo;.bar:Ljava/lang/Class;");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const-class "LBar;")
      (move-result-pseudo-object v0)
      (sput v0 "LFoo;.bar:Ljava/lang/Class;")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto store1 = DexStore("store1");
  store1.add_classes({cls});
  auto store2 = DexStore("store2");
  store2.add_classes({cls2});
  DexStoresVector stores{store1, store2};
  auto scope = build_class_scope(stores);
  XStoreRefs xstores(stores);
  FinalInlinePassV2::run(scope, &xstores);

  EXPECT_NE(cls->get_clinit(), nullptr);
  EXPECT_EQ(field->get_static_value()->evtype(), DEVT_NULL);
}

TEST_F(FinalInlineTest, fieldSetInLoop) {
  auto field_bar = create_field_with_zero_value("LFoo;.bar:I");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (:loop)
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (add-int/lit8 v0 v0 1)
      (sput v0 "LFoo;.bar:I")
      (const v1 10)
      (if-ne v0 v1 :loop)
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls}, /* xstores */ nullptr);
  EXPECT_EQ(assembler::to_s_expr(cls->get_clinit()->get_code()), original);
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
}

TEST_F(FinalInlineTest, fieldConditionallySet) {
  auto field_bar = create_field_with_zero_value("LFoo;.bar:I");
  auto field_baz = create_field_with_zero_value("LFoo;.baz:I");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (sget "LUnknown;.field:I")
      (move-result-pseudo v0)
      (if-eqz v0 :true)
      (const v1 1)
      (sput v1 "LFoo;.bar:I")
      (:true)
      ; bar may be 0 or 1 here
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (sput v0 "LFoo;.baz:I")
      (sput v1 "LFoo;.bar:I")
      ; bar is always 1 on exit
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls}, /* xstores */ nullptr);
  EXPECT_EQ(assembler::to_s_expr(cls->get_clinit()->get_code()), original);
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
  EXPECT_EQ(field_baz->get_static_value()->value(), 0);
}

TEST_F(FinalInlineTest, dominatedSget) {
  auto field_bar = create_field_with_zero_value("LFoo;.bar:I");
  auto field_baz = create_field_with_zero_value("LFoo;.baz:I");
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (sput v0 "LFoo;.baz:I")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  // This could be further optimized to remove the sput to the field bar. This
  // test illustrates that we are being overly conservative if a field is
  // ever read in its <clinit>. In practice though this rarely occurs.
  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (return-void)
    )
  )");

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls}, /* xstores */ nullptr);
  EXPECT_CODE_EQ(cls->get_clinit()->get_code(), expected.get());
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
  EXPECT_EQ(field_baz->get_static_value()->value(), 1);
}
