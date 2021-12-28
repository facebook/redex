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

class ProguardMethodObfuscationTest : public RedexTest {};
/**
 * Check renaming has been properly applied on methods.
 */
TEST_F(ProguardMethodObfuscationTest, obfuscation) {
  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  const char* configuration_file = std::getenv("pg_config_e2e_pgconfig");
  const char* refl_strategy = std::getenv("reflection_strategy");
  ASSERT_NE(nullptr, dexfile);
  ASSERT_NE(nullptr, mapping_file);
  ASSERT_NE(nullptr, configuration_file);
  ASSERT_NE(nullptr, refl_strategy);

  ProguardObfuscationTest tester(dexfile, mapping_file);
  ASSERT_TRUE(tester.configure_proguard(configuration_file))
      << "Proguard configuration failed";

  // Make sure the fields class Alpha are renamed.
  const std::string alphaName = "Lcom/facebook/redex/test/proguard/Alpha;";
  auto alpha = tester.find_class_named(alphaName);
  ASSERT_NE(nullptr, alpha);

  // Uncomment to test vmethods
  /*const std::array<std::string, 3> alphaMethodsRenamed = {
    ".doubleWombat:()I",
    ".doubleWombat:(I)I",
    ".tripleWombat:()I" };*/
  std::vector<std::string> renamed = {".unreflectedI4:()V", ".someDmethod:()I",
                                      ".anotherDmethod:(I)V",
                                      ".privateDmethod:()I"};
  const std::vector<std::string> reflected = {
      ".reflectedI1:()V", ".reflectedI2:()V", ".reflectedI3:()V",
      ".reflected1:()V",  ".reflected2:()V",  ".reflected3:()V",
      ".reflected4:()V",  ".reflected5:()V",  ".reflected6:()V"};
  if (!strcmp("rename", refl_strategy)) {
    renamed.insert(renamed.end(), reflected.begin(), reflected.end());
  } else {
    for (const std::string& methodName : reflected) {
      ASSERT_FALSE(tester.method_is_renamed(alpha, alphaName + methodName))
          << alphaName + methodName << " obfuscated";
    }
  }
  for (const std::string& methodName : renamed) {
    ASSERT_TRUE(tester.method_is_renamed(alpha, alphaName + methodName))
        << alphaName + methodName << " not obfuscated";
  }

  ASSERT_FALSE(tester.method_is_renamed(alpha, alphaName + ".<init>:()V"));
  ASSERT_FALSE(tester.method_is_renamed(alpha, alphaName + ".<init>:(I)V"));
  ASSERT_FALSE(tester.method_is_renamed(alpha, alphaName + ".<clinit>:()V"));

  // Make sure the fields in the class Beta are not renamed.
  const std::string betaName = "Lcom/facebook/redex/test/proguard/Beta;";
  auto beta = tester.find_class_named(betaName);
  ASSERT_NE(nullptr, beta);
  ASSERT_FALSE(
      tester.method_is_renamed(beta, betaName + ".doubleWombatBeta:()I"));
  ASSERT_FALSE(tester.method_is_renamed(beta, betaName + ".<init>:()V"));
}
