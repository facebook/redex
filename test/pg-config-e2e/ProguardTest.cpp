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

/**
The objective of these tests are to make sure the ProGuard rules are
properly applied to a set of test classes. Currently, this is testing
ProGuard itself since the incoming dex files have been processed with
ProGuard to perform the keep rules. However, if the ProGuard step is
omitted then these tests act on the keep matching functionality in Redex.
**/

ProguardMap* proguard_map;

DexClass* find_class_named(const DexClasses& classes, const std::string name) {
  auto mapped_search_name = std::string(proguard_map->translate_class(name));
  auto it = std::find_if(classes.begin(),
                         classes.end(),
                         [&mapped_search_name](DexClass* cls) {
                           return mapped_search_name ==
                                  std::string(cls->get_name()->c_str());
                         });
  if (it == classes.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

DexMethod* find_vmethod_named(const DexClass* cls, const std::string name) {
  auto vmethods = cls->get_vmethods();
  auto mapped_search_name = std::string(proguard_map->translate_class(name));
  auto it = std::find_if(vmethods.begin(),
                         vmethods.end(),
                         [&mapped_search_name](DexMethod* m) {
                           return mapped_search_name ==
                                  std::string(m->get_name()->c_str());
                         });
  return it == vmethods.end() ? nullptr : *it;
}

DexField* find_instance_field_named(const DexClass* cls, const char* name) {
  auto fields = cls->get_ifields();
  auto mapped_search_name = std::string(proguard_map->translate_class(name));
  auto it = std::find_if(fields.begin(),
                         fields.end(),
                         [&mapped_search_name](DexField* f) {
                           return mapped_search_name ==
                                  std::string(f->get_name()->c_str());
                         });
  return it == fields.end() ? nullptr : *it;
}

/**
 * Ensure the ProGuard test rules are properly applied.
 */
TEST(ProguardTest, assortment) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("pg_config_e2e_dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexClasses> dexen;
  dexen.emplace_back(load_classes_from_dex(dexfile));
  DexClasses& classes = dexen.back();
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  // Load the Proguard map
  const char* mapping_file = std::getenv("pg_config_e2e_mapping");
  ASSERT_NE(nullptr, mapping_file);
  proguard_map = new ProguardMap(std::string(mapping_file));

  const char* configuraiton_file = std::getenv("pg_config_e2e_pgconfig");
  ASSERT_NE(nullptr, configuraiton_file);
  redex::ProguardConfiguration pg_config;
  redex::proguard_parser::parse_file(configuraiton_file, &pg_config);
  ASSERT_TRUE(pg_config.ok);

  Scope scope = build_class_scope(dexen);
  process_proguard_rules(pg_config, proguard_map, scope);

  { // Alpha is explicitly used and should not be deleted.
    auto alpha =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Alpha;");
    ASSERT_NE(alpha, nullptr);
    ASSERT_FALSE(keep(alpha));
    ASSERT_FALSE(keepclassmembers(alpha));
    ASSERT_FALSE(keepclasseswithmembers(alpha));
  }

  // Beta is not used and should not occur in the input.
  ASSERT_EQ(
      find_class_named(classes, "Lcom/facebook/redex/test/proguard/Beta;"),
      nullptr);

  { // Gamma is not used anywhere but is kept by the config.
    auto gamma =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Gamma;");
    ASSERT_NE(gamma, nullptr);
    ASSERT_TRUE(keep(gamma));
  }

  { // Inner class Delta.A should be removed.
    auto delta_a =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$A;");
    ASSERT_EQ(delta_a, nullptr);
  }

  { // Inner class Delta.B is preserved by a keep directive.
    auto delta_b =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$B;");
    ASSERT_NE(delta_b, nullptr);
  }

  { // Inner class C is kept.
    auto delta_c =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$C;");
    ASSERT_NE(delta_c, nullptr);
    // Make sure its fields and methods have been kept by the "*;" directive.
    auto iField = find_instance_field_named(delta_c, "i");
    ASSERT_NE(iField, nullptr);
    auto iValue = find_vmethod_named(delta_c, "iValue");
    ASSERT_NE(iValue, nullptr);
  }

  { // Inner class D is kept.
    auto delta_d =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$D;");
    ASSERT_NE(delta_d, nullptr);
    // Make sure its fields are kept by "<fields>" but not its methods.
    auto iField = find_instance_field_named(delta_d, "i");
    ASSERT_NE(iField, nullptr);
    auto iValue = find_vmethod_named(delta_d, "iValue");
    ASSERT_EQ(iValue, nullptr);
  }

  { // Inner class E is kept.
    auto delta_e =
        find_class_named(classes, "Lcom/facebook/redex/test/proguard/Delta$E;");
    ASSERT_NE(delta_e, nullptr);
    // Make sure its methods are kept by "<methods>" but not its fields.
    auto iField = find_instance_field_named(delta_e, "i");
    ASSERT_EQ(iField, nullptr);
    auto iValue = find_vmethod_named(delta_e, "iValue");
    ASSERT_NE(iValue, nullptr);
  }

  delete g_redex;
}
