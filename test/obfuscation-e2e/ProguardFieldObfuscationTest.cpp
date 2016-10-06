/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <array>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardObfuscationTest.h"
#include "ProguardParser.h"
#include "ReachableClasses.h"
#include "RedexContext.h"

/**
 * Check renaming has been properly applied.
 */
TEST(ProguardTest, obfuscation) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  const char* configuration_file = std::getenv("pg_config_e2e_pgconfig");
  ASSERT_NE(nullptr, dexfile);
  ASSERT_NE(nullptr, mapping_file);
  ASSERT_NE(nullptr, configuration_file);

  ProguardObfuscationTest tester(dexfile, mapping_file);
  ASSERT_TRUE(tester.configure_proguard(configuration_file))
    << "Proguard configuration failed";

  // Make sure the fields class Alpha are renamed.
  const std::string alphaName = "Lcom/facebook/redex/test/proguard/Alpha;";
  const std::array<std::string, 4> fieldNames = {
    ".wombat:I",
    ".numbat:I",
    ".omega:Ljava/lang/String;",
    ".theta:Ljava/util/List;"};
  auto alpha = tester.find_class_named(alphaName);
  ASSERT_NE(nullptr, alpha);

  for (const std::string &fieldName : fieldNames) {
    ASSERT_TRUE(tester.field_is_renamed(
        alpha->get_ifields(),
        alphaName + fieldName)) << alphaName + fieldName << " not obfuscated";
  }

  // Make sure the fields in the class Beta are not renamed.
  auto beta = tester.find_class_named(
    "Lcom/facebook/redex/test/proguard/Beta;");
  ASSERT_NE(nullptr, beta);
  ASSERT_FALSE(tester.field_is_renamed(
      beta->get_ifields(),
      "Lcom/facebook/redex/test/proguard/Beta;.wombatBeta:I"));

  delete g_redex;
}
