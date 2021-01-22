/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <sstream>

#include "AccessMarking.h"
#include "RedexTest.h"
#include "Show.h"

class AccessMarkingTest : public RedexIntegrationTest {};

TEST_F(AccessMarkingTest, test_all) {
  std::vector<Pass*> passes = {
      new AccessMarkingPass(),
  };

  run_passes(passes);

  // Check finalization of fields

  std::string name;
  DexFieldRef* field_ref;
  DexField* field;

  name = "Lcom/facebook/redextest/TestClass;.finalizable:I";
  field_ref = DexField::get_field(name);
  ASSERT_TRUE(field_ref) << name << " not found.";
  field = field_ref->as_def();
  ASSERT_TRUE(field_ref) << name << " not a def.";
  ASSERT_TRUE(is_final(field)) << name << " not final.";

  name = "Lcom/facebook/redextest/TestClass;.not_finalizable:I";
  field_ref = DexField::get_field(name);
  ASSERT_TRUE(field_ref) << name << " not found.";
  field = field_ref->as_def();
  ASSERT_TRUE(field_ref) << name << " not a def.";
  ASSERT_FALSE(is_final(field)) << name << " final.";

  name = "Lcom/facebook/redextest/TestClass;.static_finalizable:I";
  field_ref = DexField::get_field(name);
  ASSERT_TRUE(field_ref) << name << " not found.";
  field = field_ref->as_def();
  ASSERT_TRUE(field_ref) << name << " not a def.";
  ASSERT_TRUE(is_final(field)) << name << " not final.";

  name = "Lcom/facebook/redextest/TestClass;.static_not_finalizable:I";
  field_ref = DexField::get_field(name);
  ASSERT_TRUE(field_ref) << name << " not found.";
  field = field_ref->as_def();
  ASSERT_TRUE(field_ref) << name << " not a def.";
  ASSERT_FALSE(is_final(field)) << name << " final.";

  name = "Lcom/facebook/redextest/TestClass;.static_not_finalizable2:I";
  field_ref = DexField::get_field(name);
  ASSERT_TRUE(field_ref) << name << " not found.";
  field = field_ref->as_def();
  ASSERT_TRUE(field_ref) << name << " not a def.";
  ASSERT_FALSE(is_final(field)) << name << " final.";
}
