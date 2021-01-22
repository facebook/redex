/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <json/json.h>
#include <sstream>

#include "AccessMarking.h"
#include "RedexTest.h"
#include "Show.h"

class AccessMarkingTest : public RedexIntegrationTest {};

TEST_F(AccessMarkingTest, test_all) {
  std::vector<Pass*> passes = {
      new AccessMarkingPass(),
  };

  auto config_file_env = std::getenv("config_file");
  always_assert_log(config_file_env,
                    "Config file must be specified to AccessMarkingTest.\n");

  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;

  run_passes(passes, nullptr, cfg);

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
