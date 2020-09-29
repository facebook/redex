/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ProguardConfiguration.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "RedexTest.h"

#include "Reachability.h"

// TODO(T68802519)(adicatana): the helper functions below are now being used in
// 3 different places, add to RedexIntegrationTest maybe.
class ReachabilityTest : public RedexIntegrationTest {};

std::unique_ptr<keep_rules::ProguardConfiguration>
process_and_get_proguard_config(const std::vector<DexClasses>& dexen,
                                const std::string& config) {
  auto pg_config = std::make_unique<keep_rules::ProguardConfiguration>();
  std::istringstream pg_config_text(config);
  keep_rules::proguard_parser::parse(pg_config_text, pg_config.get());

  ProguardMap pm;
  // We aren't loading any external jars for this test
  // so external_classes is empty
  Scope external_classes;
  apply_deobfuscated_names(dexen, pm);
  Scope scope = build_class_scope(dexen);
  process_proguard_rules(pm, scope, external_classes, *pg_config, true);
  return pg_config;
}

TEST_F(ReachabilityTest, ReachabilityFromProguardTest) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testMethod();
    }
    -keepclasseswithmembers class A {
      int foo;
      <init>();
      int bar();
    }
  )");

  EXPECT_TRUE(pg_config->ok);
  EXPECT_EQ(pg_config->keep_rules.size(), 2);

  reachability::ObjectCounts before = reachability::count_objects(stores);

  EXPECT_EQ(before.num_classes, 19);
  EXPECT_EQ(before.num_methods, 35);
  EXPECT_EQ(before.num_fields, 3);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings);

  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 7);
  EXPECT_EQ(after.num_methods, 13);
  EXPECT_EQ(after.num_fields, 2);
}

TEST_F(ReachabilityTest, ReachabilityMarkAllTest) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testMethod();
    }
    -keepclasseswithmembers class A {
      int foo;
      <init>();
      int bar();
    }
  )");

  EXPECT_TRUE(pg_config->ok);
  EXPECT_EQ(pg_config->keep_rules.size(), 2);

  reachability::ObjectCounts before = reachability::count_objects(stores);

  EXPECT_EQ(before.num_classes, 19);
  EXPECT_EQ(before.num_methods, 35);
  EXPECT_EQ(before.num_fields, 3);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, false, true, nullptr);

  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 19);
  EXPECT_EQ(after.num_methods, 35);
  EXPECT_EQ(after.num_fields, 3);
}
