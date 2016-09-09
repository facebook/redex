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

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Match.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "ReachableClasses.h"
#include "RedexContext.h"

/**
The objective of these tests are to make sure the ProGuard rules are
properly applied to a set of test classes. The incomming APK is currently
already processed by ProGuard. This test makes sure the expected classes
and methods are present (or absent) as required and performs checks on the
Redex ProGuard rule matcher to make sure the ProGuard rules were properly
interpreted.
**/

DexClass* find_class_named(const ProguardMap& proguard_map,
                           const DexClasses& classes,
                           const std::string name) {
  auto mapped_search_name = std::string(proguard_map.translate_class(name));
  auto it = std::find_if(
      classes.begin(), classes.end(), [&mapped_search_name](DexClass* cls) {
        return mapped_search_name == std::string(cls->c_str());
      });
  if (it == classes.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

// Returns true if the specified field is not renamed.
bool field_is_not_renamed(const ProguardMap& proguard_map,
                          const std::list<DexField*> fields,
                          const std::string name) {
  auto it = std::find_if(
      fields.begin(), fields.end(), [&name, &proguard_map](DexField* f) {
        auto deobfuscated_name =
            proguard_map.deobfuscate_field(proguard_name(f));
        if (name == std::string(f->c_str()) || (name == deobfuscated_name)) {
          return deobfuscated_name == name;
        }
        return false;
      });
  return it != fields.end();
}

/**
 * Check renaming has been properly applied.
 */
TEST(ProguardTest, obfuscation) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();

  // Load the Proguard map
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  ASSERT_NE(nullptr, mapping_file);
  ProguardMap proguard_map((std::string(mapping_file)));

  const char* configuraiton_file = std::getenv("pg_config_e2e_pgconfig");
  ASSERT_NE(nullptr, configuraiton_file);
  redex::ProguardConfiguration pg_config;
  redex::proguard_parser::parse_file(configuraiton_file, &pg_config);
  ASSERT_TRUE(pg_config.ok);

  Scope scope = build_class_scope(dexen);
  apply_deobfuscated_names(dexen, proguard_map);
  process_proguard_rules(pg_config, &proguard_map, scope);

  // Make sure the fields class Alpha are renamed.
  auto alpha = find_class_named(
      proguard_map, classes, "Lcom/facebook/redex/test/proguard/Alpha;");
  ASSERT_NE(alpha, nullptr);
  ASSERT_TRUE(field_is_not_renamed(
      proguard_map,
      alpha->get_ifields(),
      "Lcom/facebook/redex/test/proguard/Alpha;.wombat:I"));

  // Make sure the fields in the class Beta are not renamed.
  auto beta = find_class_named(
      proguard_map, classes, "Lcom/facebook/redex/test/proguard/Beta;");
  ASSERT_NE(beta, nullptr);
  ASSERT_FALSE(field_is_not_renamed(
      proguard_map,
      alpha->get_ifields(),
      "Lcom/facebook/redex/test/proguard/Beta;.wombatBeta:I"));

  delete g_redex;
}
