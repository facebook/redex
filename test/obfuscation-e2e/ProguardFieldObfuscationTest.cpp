/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <array>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexClass.h"
#include "DexLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardObfuscationTest.h"
#include "ProguardParser.h"
#include "ReachableClasses.h"
#include "RedexTest.h"

void testClass(ProguardObfuscationTest* tester,
               const std::string& class_name,
               const std::vector<std::string>& fields,
               bool expects_found = false) {
  auto clazz = tester->find_class_named(class_name);
  ASSERT_NE(nullptr, clazz) << class_name << " not found.";

  for (const std::string& fieldName : fields) {
    ASSERT_EQ(expects_found,
              tester->field_found(clazz->get_ifields(), class_name + fieldName))
        << class_name + fieldName << (expects_found ? "" : " not")
        << " obfuscated";
  }
}

class ProguardFieldObfuscationTest : public RedexTest {};

/**
 * Check renaming has been properly applied.
 */
TEST_F(ProguardFieldObfuscationTest, obfuscation) {
  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  const char* configuration_file = std::getenv("pg_config_e2e_pgconfig");
  const char* refl_strategy = std::getenv("reflection_strategy");
  ASSERT_NE(nullptr, dexfile);
  ASSERT_NE(nullptr, configuration_file);
  ASSERT_NE(nullptr, refl_strategy);

  ProguardObfuscationTest tester(dexfile, mapping_file);
  ASSERT_TRUE(tester.configure_proguard(configuration_file))
      << "Proguard configuration failed";

  // Make sure the fields class Alpha are renamed.
  std::vector<std::string> reflectedNames = {".reflected1:I", ".reflected2:I",
                                             ".reflected3:I", ".reflected4:J",
                                             ".reflected5:Ljava/lang/Object;"};
  std::vector<std::string> alphaNames = {
      ".wombat:I", ".numbat:I", ".reflected6:I", ".omega:Ljava/lang/String;",
      ".theta:Ljava/util/List;"};
  if (!strcmp(refl_strategy, "rename")) {
    alphaNames.insert(
        alphaNames.end(), reflectedNames.begin(), reflectedNames.end());
  } else {
    // Ensure reflectedNames are NOT renamed
    testClass(&tester,
              "Lcom/facebook/redex/test/proguard/Alpha;",
              reflectedNames,
              true);
  }
  const std::vector<std::string> helloNames = {".hello:Ljava/lang/String;"};
  const std::vector<std::string> worldNames = {".world:Ljava/lang/String;"};
  testClass(&tester, "Lcom/facebook/redex/test/proguard/Alpha;", alphaNames);
  testClass(&tester, "Lcom/facebook/redex/test/proguard/Hello;", helloNames);
  testClass(&tester, "Lcom/facebook/redex/test/proguard/World;", worldNames);

  // Because of the all() call in Beta, there should be refs created in the
  // bytecode of all() to All.hello and All.world which should be updated
  // to Hello.[renamed] and World.[renamed]
  ASSERT_FALSE(tester.refs_to_field_found(helloNames[0]))
      << "Refs to " << helloNames[0] << " not properly modified";
  ASSERT_FALSE(tester.refs_to_field_found(worldNames[0]))
      << "Refs to " << worldNames[0] << " not properly modified";

  // Make sure the fields in the class Beta are not renamed.
  auto beta =
      tester.find_class_named("Lcom/facebook/redex/test/proguard/Beta;");
  ASSERT_NE(nullptr, beta);
  ASSERT_TRUE(tester.field_found(
      beta->get_ifields(),
      "Lcom/facebook/redex/test/proguard/Beta;.wombatBeta:I"));
}
