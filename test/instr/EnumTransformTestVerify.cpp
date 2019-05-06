/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "EnumTransformer.h"
#include "verify/VerifyUtil.h"

using namespace optimize_enums;

namespace {
struct EnumUtil {
  const char* enum_score_class_name = "Lcom/facebook/redextest/SCORE;";
  const char* enum_pure_score_class_name =
      "Lcom/facebook/redextest/PURE_SCORE;";
  const char* array_name = "array";
  const char* class_name = "Lcom/facebook/redextest/C;";
  const char* substitute_array_name = "array$RDX$uCTBV1V51xg";
};

void expect_other_enums(const DexClasses& classes) {
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_WHEN_RETURN;"),
      nullptr);
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_THIS_POINTER;"),
      nullptr);
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_PARAMETER;"),
      nullptr);
  EXPECT_NE(find_class_named(classes,
                             "Lcom/facebook/redextest/USED_AS_CLASS_OBJECT;"),
            nullptr);
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_CHECK_CAST;"),
      nullptr);
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_ISPUT_OBJECT;"),
      nullptr);
  EXPECT_NE(
      find_class_named(classes, "Lcom/facebook/redextest/CAST_APUT_OBJECT;"),
      nullptr);
}
} // namespace

TEST_F(PreVerify, transform) {
  EnumUtil util;
  // SCORE enum class
  auto enum_cls = find_class_named(classes, util.enum_score_class_name);
  EXPECT_NE(enum_cls, nullptr);
  EXPECT_EQ(enum_cls->get_super_class(), get_enum_type());
  EXPECT_EQ(enum_cls->get_access() & ACC_ENUM, ACC_ENUM);
  EXPECT_EQ(enum_cls->get_sfields().size(), 4);
  // An SCORE[][] field
  EXPECT_NE(DexField::get_field(DexType::get_type(util.class_name),
                                DexString::make_string(util.array_name),
                                make_array_type(enum_cls->get_type(), 2)),
            nullptr);
  EXPECT_EQ(
      DexField::get_field(DexType::get_type(util.class_name),
                          DexString::make_string(util.substitute_array_name),
                          make_array_type(enum_cls->get_type(), 2)),
      nullptr);
  // PURE_SCORE enum class.
  enum_cls = find_class_named(classes, util.enum_pure_score_class_name);
  EXPECT_NE(enum_cls, nullptr);
  EXPECT_EQ(enum_cls->get_sfields().size(), 4);
  // Other enums
  expect_other_enums(classes);
}

TEST_F(PostVerify, transform) {
  EnumUtil util;
  // SCORE class is optimized.
  auto enum_cls = find_class_named(classes, util.enum_score_class_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_EQ(enum_cls->get_super_class(), get_object_type());
  EXPECT_EQ(enum_cls->get_access() & ACC_ENUM, 0);
  EXPECT_EQ(enum_cls->get_sfields().size(), 0);
  // EnumUtil
  auto util_cls = find_class_named(classes, "Lredex/$EnumUtils;");
  EXPECT_NE(util_cls, nullptr);
  // SCORE and PURE_SCORE are optimized, number of generated static fields
  // should be at least 4. Note that some enum classes from support library may
  // be optimized so that the number of generated static fields may be greater
  // than 4.
  EXPECT_GE(util_cls->get_sfields().size(), 4);
  // A SCORE[][] field => An Integer[][] field.
  EXPECT_EQ(DexField::get_field(DexType::get_type(util.class_name),
                                DexString::make_string(util.array_name),
                                make_array_type(enum_cls->get_type(), 2)),
            nullptr);
  auto array_field =
      DexField::get_field(DexType::get_type(util.class_name),
                          DexString::make_string(util.substitute_array_name),
                          make_array_type(get_integer_type(), 2));
  EXPECT_NE(array_field, nullptr);

  // PURE_SCORE enum class is optimized and deleted.
  EXPECT_EQ(find_class_named(classes, util.enum_pure_score_class_name),
            nullptr);

  // Other enums are not optimized.
  expect_other_enums(classes);
}
