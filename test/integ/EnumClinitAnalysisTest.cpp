/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumClinitAnalysis.h"

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "JarLoader.h"
#include "RedexTest.h"

constexpr const char* ENUM_SAFE = "Lcom/facebook/redextest/EnumSafe;";
constexpr const char* ENUM_SAFE_A =
    "Lcom/facebook/redextest/EnumSafe;.A:Lcom/facebook/redextest/EnumSafe;";
constexpr const char* ENUM_SAFE_B =
    "Lcom/facebook/redextest/EnumSafe;.B:Lcom/facebook/redextest/EnumSafe;";
constexpr const char* ENUM_SAFE_NAME =
    "Lcom/facebook/redextest/EnumSafe;.name:Ljava/lang/String;";
constexpr const char* ENUM_SAFE_VALUE =
    "Lcom/facebook/redextest/EnumSafe;.value:I";
constexpr const char* ENUM_SAFE_IS_USEFUL =
    "Lcom/facebook/redextest/EnumSafe;.isUseful:Z";

class EnumClinitAnalysisTest : public RedexIntegrationTest {};

/*
 * Check that analyze_enum_clinit returns the correct enum field -> ordinal and
 * name mapping.
 */
TEST_F(EnumClinitAnalysisTest, OrdinalAnalysis) {
  using namespace optimize_enums;
  ASSERT_TRUE(load_class_file(std::getenv("enum_class_file")));

  // EnumSafe
  auto enum_cls = type_class(DexType::get_type(ENUM_SAFE));
  auto attributes = analyze_enum_clinit(enum_cls);
  auto& enum_constants = attributes.m_constants_map;
  auto& ifield_map = attributes.m_field_map;

  EXPECT_EQ(enum_constants.size(), 2);
  EXPECT_EQ(ifield_map.size(), 3);

  auto field = static_cast<DexField*>(DexField::get_field(ENUM_SAFE_A));
  ASSERT_EQ(enum_constants.count(field), 1);
  EXPECT_EQ(enum_constants[field].ordinal, 0);
  EXPECT_EQ(enum_constants[field].name, DexString::make_string("A"));

  field = static_cast<DexField*>(DexField::get_field(ENUM_SAFE_B));
  ASSERT_EQ(enum_constants.count(field), 1);
  EXPECT_EQ(enum_constants[field].ordinal, 1);
  EXPECT_EQ(enum_constants[field].name, DexString::make_string("B"));

  auto ifield = DexField::get_field(ENUM_SAFE_NAME);
  ASSERT_EQ(ifield_map.count(ifield), 1);
  ASSERT_EQ(ifield_map[ifield].size(), 2);
  EXPECT_EQ(ifield_map[ifield][0].string_value, DexString::make_string("zero"));
  EXPECT_EQ(ifield_map[ifield][1].string_value, nullptr);

  ifield = DexField::get_field(ENUM_SAFE_VALUE);
  ASSERT_EQ(ifield_map.count(ifield), 1);
  ASSERT_EQ(ifield_map[ifield].size(), 2);
  EXPECT_EQ(ifield_map[ifield][0].primitive_value, 0);
  EXPECT_EQ(ifield_map[ifield][1].primitive_value, 1);

  ifield = DexField::get_field(ENUM_SAFE_IS_USEFUL);
  ASSERT_EQ(ifield_map.count(ifield), 1);
  ASSERT_EQ(ifield_map[ifield].size(), 2);
  EXPECT_EQ(ifield_map[ifield][0].primitive_value, 1);
  EXPECT_EQ(ifield_map[ifield][1].primitive_value, 1);

  // These enums should not be optimized.
  for (const char* enum_name : {"Lcom/facebook/redextest/EnumUnsafe1;",
                                "Lcom/facebook/redextest/EnumUnsafe2;"}) {
    enum_cls = type_class(DexType::get_type(enum_name));
    attributes = analyze_enum_clinit(enum_cls);
    EXPECT_TRUE(attributes.m_constants_map.empty());
    EXPECT_TRUE(attributes.m_field_map.empty());
  }
}
