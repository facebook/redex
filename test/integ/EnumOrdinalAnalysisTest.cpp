/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "EnumOrdinalAnalysis.h"

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "JarLoader.h"
#include "RedexTest.h"

constexpr const char* ENUM_A = "Lcom/facebook/redextest/EnumA;";
constexpr const char* ENUM_B = "Lcom/facebook/redextest/EnumB;";

/*
 * Check that analyze_enum_clinit returns the correct enum field -> ordinal
 * mapping.
 */
TEST_F(RedexTest, OrdinalAnalysis) {
  always_assert(load_class_file(std::getenv("enum_class_file")));

  auto dexfile = std::getenv("dexfile");
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  auto scope = build_class_scope(root_store.get_dexen());

  auto enumA = type_class(DexType::get_type(ENUM_A));
  auto enum_field_to_ordinal = optimize_enums::analyze_enum_clinit(enumA);
  auto enumA_zero = static_cast<DexField*>(
      DexField::get_field("Lcom/facebook/redextest/EnumA;.TYPE_A_0:Lcom/"
                          "facebook/redextest/EnumA;"));
  auto enumA_one = static_cast<DexField*>(
      DexField::get_field("Lcom/facebook/redextest/EnumA;.TYPE_A_1:Lcom/"
                          "facebook/redextest/EnumA;"));
  auto enumA_two = static_cast<DexField*>(
      DexField::get_field("Lcom/facebook/redextest/EnumA;.TYPE_A_2:Lcom/"
                          "facebook/redextest/EnumA;"));
  EXPECT_EQ(enum_field_to_ordinal.at(enumA_zero), 0);
  EXPECT_EQ(enum_field_to_ordinal.at(enumA_one), 1);
  EXPECT_EQ(enum_field_to_ordinal.at(enumA_two), 2);
}
