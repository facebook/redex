/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardParser.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ReachableClasses.h"
#include "RedexContext.h"


ProguardMap* proguard_map;

DexClass* find_class_named(const DexClasses& classes, const std::string name) {
  auto it = std::find_if(classes.begin(), classes.end(), [&name](DexClass* cls){
    return proguard_map->translate_class(name) == std::string(cls->get_name()->c_str());
  });
  if (it == classes.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

/**
 * Ensure the ProGuard test rules are properly applied.
 */
TEST(ProguardTest, assortment) {
  g_redex = new RedexContext();

  // For testing with XCode set the working directory to the buck-out directory.
  const char* dexfile = "gen/native/redex/test/proguard/dex_pre/classes.pre.dex";
  if (access(dexfile, R_OK) != 0) {
    dexfile = std::getenv("dexfile");
  }
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl ;

  // Load the Proguard map
  const char* mapping_file = "gen/native/redex/test/proguard/mapping/mapping.txt";
  if (access(mapping_file, R_OK) != 0) {
    mapping_file = std::getenv("mapping");
  }
  ASSERT_NE(nullptr, mapping_file);
  proguard_map = new ProguardMap(std::string(mapping_file));

  const char* configuraiton_file = "gen/native/redex/test/proguard/pg-config";
  if (access(configuraiton_file, R_OK) != 0) {
    configuraiton_file = std::getenv("pgconfig");
  }
  redex::ProguardConfiguration pg_config;
  redex::proguard_parser::parse_file(configuraiton_file, &pg_config);
  ASSERT_TRUE(pg_config.ok);

  Scope scope = build_class_scope(dexen);
  process_proguard_rules(pg_config, proguard_map, scope);

  // Alpha is explicitly used and should not be deleted.
  auto alpha = find_class_named(classes, "Lcom/facebook/redex/test/proguard/Alpha;");
  ASSERT_NE(alpha, nullptr);
  ASSERT_FALSE(keep(alpha));
  ASSERT_FALSE(keepclassmembers(alpha));
  ASSERT_FALSE(keepclasseswithmembers(alpha));

  // Beta is not used and should not occur in the input.
  ASSERT_EQ(find_class_named(classes, "Lcom/facebook/redex/test/proguard/Beta;"), nullptr);

  // Gamma is not used anywhere but is kept by the config.
	auto gamma = find_class_named(classes, "Lcom/facebook/redex/test/proguard/Gamma;");
  ASSERT_NE(gamma, nullptr);
	ASSERT_TRUE(keep(gamma));

  delete g_redex;
}
