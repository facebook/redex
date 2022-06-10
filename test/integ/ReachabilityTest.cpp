/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Reachability.h"
#include "RedexTest.h"

class ReachabilityTest : public RedexIntegrationTest {};

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
      stores, ig_sets, &num_ignore_check_strings,
      /* record_reachability */ false, /* should_mark_all_as_seed */ true,
      nullptr);

  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 19);
  EXPECT_EQ(after.num_methods, 35);
  EXPECT_EQ(after.num_fields, 3);
}

TEST_F(ReachabilityTest, MarkAndSweepFromSeedsTest) {
  // Make sure some unreachable things exist before we start.
  auto* m1 = find_vmethod(*classes, "LA;", "V", "bor", {});
  auto* m2 = find_vmethod(*classes, "LD;", "V", "bor", {});

  EXPECT_NE(m1, nullptr);
  EXPECT_NE(m2, nullptr);

  reachability::ObjectCounts before = reachability::count_objects(stores);

  EXPECT_EQ(before.num_classes, 19);
  EXPECT_EQ(before.num_methods, 35);
  EXPECT_EQ(before.num_fields, 3);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  std::unordered_set<const DexMethod*> seeds{m1, m2};

  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, seeds, false, nullptr);

  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 3);
  EXPECT_EQ(after.num_methods, 5);
  EXPECT_EQ(after.num_fields, 2);
}

TEST_F(ReachabilityTest, MethodReachabilityProguardTest) {
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

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, false, false, nullptr);

  auto reachable_methods =
      reachability::compute_reachable_methods(stores, *reachable_objects);

  std::unordered_set<std::string> reachable_method_names;
  for (const auto* m : reachable_methods) {
    reachable_method_names.insert(m->get_fully_deobfuscated_name());
  }

  EXPECT_EQ(reachable_methods.size(), 13);

  // Reachable from LA.bor
  EXPECT_EQ(reachable_method_names.count("LA;.<init>:()V"), 1);
  EXPECT_EQ(reachable_method_names.count("LA;.bar:()I"), 1);
  EXPECT_EQ(reachable_method_names.count("LA;.bor:()V"), 0);

  // Reachable from LA.bor->LA.<init>
  EXPECT_EQ(reachable_method_names.count("LOnlyInArray;.<init>:()V"), 1);

  // Reachable from LD.bor
  EXPECT_EQ(reachable_method_names.count("LD;.<init>:()V"), 1);
  EXPECT_EQ(reachable_method_names.count("LD;.bar:()I"), 1);
  EXPECT_EQ(reachable_method_names.count("LD;.bor:()V"), 0);
}

TEST_F(ReachabilityTest, FieldReachabilityProguardTest) {
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

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, false, false, nullptr);

  auto reachable_fields =
      reachability::compute_reachable_fields(stores, *reachable_objects);

  std::unordered_set<std::string> reachable_fields_names;
  for (const auto* f : reachable_fields) {
    reachable_fields_names.insert(f->get_deobfuscated_name_or_empty());
  }

  EXPECT_EQ(reachable_fields.size(), 2);

  // Reachable from LA.<init>
  EXPECT_EQ(reachable_fields_names.count("LA;.arr:[LOnlyInArray;"), 1);
  EXPECT_EQ(reachable_fields_names.count("LA;.foo:I"), 1);
}

TEST_F(ReachabilityTest, MethodReachabilityFromSeedsTest) {
  // Make sure some unreachable things exist before we start.
  auto* m1 = find_vmethod(*classes, "LA;", "V", "bor", {});
  auto* m2 = find_vmethod(*classes, "LD;", "V", "bor", {});

  EXPECT_NE(m1, nullptr);
  EXPECT_NE(m2, nullptr);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;

  std::unordered_set<const DexMethod*> seeds{m1, m2};

  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, seeds, false, nullptr);

  auto reachable_methods =
      reachability::compute_reachable_methods(stores, *reachable_objects);

  std::unordered_set<std::string> reachable_method_names;
  for (const auto* m : reachable_methods) {
    reachable_method_names.insert(m->get_fully_deobfuscated_name());
  }

  EXPECT_EQ(reachable_methods.size(), 5);

  // Reachable from LA.bor
  EXPECT_EQ(reachable_method_names.count("LA;.<init>:()V"), 1);
  EXPECT_EQ(reachable_method_names.count("LA;.bor:()V"), 1);

  // Reachable from LA.bor->LA.<init>
  EXPECT_EQ(reachable_method_names.count("LOnlyInArray;.<init>:()V"), 1);

  // Reachable from LD.bor
  EXPECT_EQ(reachable_method_names.count("LD;.<init>:()V"), 1);
  EXPECT_EQ(reachable_method_names.count("LD;.bor:()V"), 1);
}

TEST_F(ReachabilityTest, ReachabilitySkipMethodsTest) {
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

  reachability::IgnoreSets ig_sets;
  auto num_ignore_check_strings = 0;
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, false);

  reachability::ObjectCounts before = reachability::count_objects(stores);

  EXPECT_EQ(before.num_classes, 19);
  EXPECT_EQ(before.num_methods, 35);
  EXPECT_EQ(before.num_fields, 3);

  // Make sure some unreachable things exist before we start.
  auto* method = find_vmethod(*classes, "LA;", "I", "bar", {});
  EXPECT_NE(method, nullptr);

  ig_sets.methods.insert(method);
  reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, false);

  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 7);
  EXPECT_EQ(after.num_methods, 10);
  EXPECT_EQ(after.num_fields, 2);

  method = find_vmethod(*classes, "LA;", "I", "bar", {});
  EXPECT_EQ(method, nullptr);

  method = find_vmethod(*classes, "LD;", "I", "bar", {});
  EXPECT_NE(method, nullptr);
}
