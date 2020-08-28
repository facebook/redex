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
template <typename C>
DexClass* find_class(const C& classes, const std::string& name) {
  const auto it =
      std::find_if(classes.begin(), classes.end(),
                   [&name](const DexClass* cls) { return cls->str() == name; });
  return it == classes.end() ? nullptr : *it;
}

template <typename C>
DexField* find_ifield(const C& classes,
                      const char* cls,
                      const char* type,
                      const char* name) {
  const auto* c = find_class(classes, cls);
  const auto& ifields = c->get_ifields();
  const auto it = std::find(ifields.begin(), ifields.end(),
                            DexField::make_field(DexType::make_type(cls),
                                                 DexString::make_string(name),
                                                 DexType::make_type(type)));
  return it == ifields.end() ? nullptr : *it;
}

template <typename C>
DexMethod* find_vmethod(const C& classes,
                        const char* cls,
                        const char* rtype,
                        const char* name,
                        const std::vector<const char*>& args) {
  const auto* c = find_class(classes, cls);
  const auto& vmethods = c->get_vmethods();
  const auto it = std::find(vmethods.begin(), vmethods.end(),
                            DexMethod::make_method(cls, name, rtype, args));
  return it == vmethods.end() ? nullptr : *it;
}

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
      stores, ig_sets, &num_ignore_check_strings, true, false, nullptr);

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
