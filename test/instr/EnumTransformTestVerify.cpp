/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "EnumClinitAnalysis.h"
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
  const char* substitute_array_name = "array$REDEX$uCTBV1V51xg";
};

/**
 * Check if there is any static enum field in the class.
 */
bool has_enum_instance(const DexClass* cls) {
  auto& sfields = cls->get_sfields();
  return std::any_of(sfields.begin(), sfields.end(), [](DexField* field) {
    return check_required_access_flags(enum_field_access(),
                                       field->get_access());
  });
}

bool is_enum_class(const DexClass* cls) {
  return cls->get_super_class() == get_enum_type() &&
         cls->get_access() & ACC_ENUM && has_enum_instance(cls);
}

void expect_other_enums(const DexClasses& classes) {
  std::vector<std::string> class_names{
      "Lcom/facebook/redextest/CAST_WHEN_RETURN;",
      "Lcom/facebook/redextest/CAST_THIS_POINTER;",
      "Lcom/facebook/redextest/CAST_PARAMETER;",
      "Lcom/facebook/redextest/USED_AS_CLASS_OBJECT;",
      "Lcom/facebook/redextest/CAST_CHECK_CAST;",
      "Lcom/facebook/redextest/CAST_ISPUT_OBJECT;",
      "Lcom/facebook/redextest/CAST_APUT_OBJECT;",
      "Lcom/facebook/redextest/ENUM_TYPE_1;",
      "Lcom/facebook/redextest/ENUM_TYPE_2;"};
  for (auto& name : class_names) {
    auto cls = find_class_named(classes, name.c_str());
    EXPECT_TRUE(is_enum_class(cls));
  }
}
} // namespace

TEST_F(PreVerify, transform) {
  EnumUtil util;
  // SCORE enum class
  auto enum_cls = find_class_named(classes, util.enum_score_class_name);
  EXPECT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum_class(enum_cls));
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
  EXPECT_TRUE(is_enum_class(enum_cls));
  // Other enums
  expect_other_enums(classes);
}

TEST_F(PostVerify, transform) {
  /*
  EnumUtil util;
  // SCORE class is optimized.
  auto enum_cls = find_class_named(classes, util.enum_score_class_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_FALSE(is_enum_class(enum_cls));
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
  */

  // Other enums are not optimized.
  expect_other_enums(classes);
}
