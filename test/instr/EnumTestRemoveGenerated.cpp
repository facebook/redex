/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "EnumClinitAnalysis.h"
#include "verify/VerifyUtil.h"

using namespace optimize_enums;

namespace {

const char* enum_usesvalueof_name = "Lcom/facebook/redextest/UsesValueOf;";
const char* enum_usesvaluesmethod_name =
    "Lcom/facebook/redextest/UsesValuesMethod;";
const char* enum_usesnothing_name = "Lcom/facebook/redextest/UsesNothing;";
const char* enum_captured_name = "Lcom/facebook/redextest/Captured;";
const char* enum_usedastypeclass_name =
    "Lcom/facebook/redextest/UsedAsTypeClass;";
const char* enum_upcasted_name = "Lcom/facebook/redextest/Upcasted;";
const char* enum_upcastedtoserializable_name =
    "Lcom/facebook/redextest/UpcastedToSerializable;";
const char* enum_instancefieldofserializable_name =
    "Lcom/facebook/redextest/InstanceFieldOfSerializable;";

bool has_valueof_method(DexType* enum_type) {
  auto proto = DexProto::get_proto(
      enum_type,
      DexTypeList::get_type_list({DexType::get_type("Ljava/lang/String;")}));
  return DexMethod::get_method(
             enum_type, DexString::make_string("valueOf"), proto) != nullptr;
}

bool has_values_method(DexType* enum_type) {
  auto proto = DexProto::get_proto(make_array_type(enum_type),
                                   DexTypeList::get_type_list({}));
  return DexMethod::get_method(
             enum_type, DexString::make_string("values"), proto) != nullptr;
}

bool has_values_field(DexType* enum_type) {
  return DexField::get_field(enum_type,
                             DexString::make_string("$VALUES"),
                             make_array_type(enum_type)) != nullptr;
}
} // namespace

/**
 * Test that the generated methods `valueOf()` and `values()` are removed if
 * safe.
 */
TEST_F(PostVerify, transform) {
  // UsesValueOf class
  auto enum_cls = find_class_named(classes, enum_usesvalueof_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // UsesValuesMethod class
  enum_cls = find_class_named(classes, enum_usesvaluesmethod_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  // EXPECT_FALSE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // UsesNothing class
  enum_cls = find_class_named(classes, enum_usesnothing_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  // EXPECT_FALSE(has_valueof_method(enum_cls->get_type()));
  // EXPECT_FALSE(has_values_method(enum_cls->get_type()));
  // EXPECT_FALSE(has_values_field(enum_cls->get_type()));

  // Captured class
  enum_cls = find_class_named(classes, enum_captured_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // UsedAsTypeClass class
  enum_cls = find_class_named(classes, enum_usedastypeclass_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // Upcasted class
  enum_cls = find_class_named(classes, enum_upcasted_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // UpcastedToSerializable class
  enum_cls = find_class_named(classes, enum_upcastedtoserializable_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));

  // InstanceOfSerializable class
  enum_cls = find_class_named(classes, enum_instancefieldofserializable_name);
  ASSERT_NE(enum_cls, nullptr);
  EXPECT_TRUE(is_enum(enum_cls));
  EXPECT_TRUE(has_valueof_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_method(enum_cls->get_type()));
  EXPECT_TRUE(has_values_field(enum_cls->get_type()));
}
