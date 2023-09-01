/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Reachability.h"
#include "RedexTest.h"
#include "Walkers.h"

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

  EXPECT_EQ(before.num_classes, 27);
  EXPECT_EQ(before.num_methods, 53);
  EXPECT_EQ(before.num_fields, 3);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  reachability::ReachableAspects reachable_aspects;
  auto scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.build_cfg(); });
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, &reachable_aspects);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });

  reachability::mark_classes_abstract(stores, *reachable_objects,
                                      reachable_aspects);
  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 7);
  EXPECT_EQ(after.num_methods, 14);
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

  EXPECT_EQ(before.num_classes, 27);
  EXPECT_EQ(before.num_methods, 53);
  EXPECT_EQ(before.num_fields, 3);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  reachability::ReachableAspects reachable_aspects;
  auto scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.build_cfg(); });
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, &reachable_aspects,
      /* record_reachability */ false, /* relaxed_keep_class_members */ false,
      /* cfg_gathering_check_instantiable */ false,
      /* cfg_gathering_check_instance_callable */ false,
      /* should_mark_all_as_seed */ true, nullptr);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });

  reachability::mark_classes_abstract(stores, *reachable_objects,
                                      reachable_aspects);
  reachability::sweep(stores, *reachable_objects, nullptr);

  reachability::ObjectCounts after = reachability::count_objects(stores);

  EXPECT_EQ(after.num_classes, 27);
  EXPECT_EQ(after.num_methods, 53);
  EXPECT_EQ(after.num_fields, 3);
}

TEST_F(ReachabilityTest, NotDireclyInstantiatedClassesBecomeAbstract) {
  // Not directly instantiated classes need to be made abstract, as we may
  // remove implementations/overrides from it.
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testUninstantiated();
    }
  )");

  EXPECT_TRUE(pg_config->ok);
  EXPECT_EQ(pg_config->keep_rules.size(), 1);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  reachability::ReachableAspects reachable_aspects;
  auto scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.build_cfg(); });
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, &reachable_aspects);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
  auto abstracted_classes = reachability::mark_classes_abstract(
      stores, *reachable_objects, reachable_aspects);
  EXPECT_EQ(abstracted_classes.size(), 1);
  reachability::sweep(stores, *reachable_objects, nullptr);

  //// instantiable_types
  EXPECT_EQ(reachable_aspects.instantiable_types.size(), 3);
  auto is_instantiable = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.instantiable_types.begin(),
                       reachable_aspects.instantiable_types.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_instantiable("LJ;"));
  EXPECT_TRUE(is_instantiable("LInstantiated;"));
  EXPECT_FALSE(is_instantiable("LUninstantiated;"));

  auto instantiated_cls = find_class(scope, "LInstantiated;");
  EXPECT_TRUE(instantiated_cls);
  EXPECT_FALSE(is_abstract(instantiated_cls));
  auto instantiated_implement_me =
      find_vmethod(*classes, "LInstantiated;", "V", "implementMe", {});
  ASSERT_TRUE(instantiated_implement_me);

  auto uninstantiated_cls = find_class(scope, "LUninstantiated;");
  EXPECT_TRUE(uninstantiated_cls);
  EXPECT_TRUE(is_abstract(uninstantiated_cls));
  auto uninstantiated_implement_me =
      find_vmethod(*classes, "LUninstantiated;", "V", "implementMe", {});
  ASSERT_FALSE(uninstantiated_implement_me);
}

TEST_F(ReachabilityTest, SharpeningCreatesMoreZombies) {
  // Not directly instantiated classes need to be made abstract, as we may
  // remove implementations/overrides from it.
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testSharpening();
    }
  )");

  EXPECT_TRUE(pg_config->ok);
  EXPECT_EQ(pg_config->keep_rules.size(), 1);

  int num_ignore_check_strings = 0;
  reachability::IgnoreSets ig_sets;
  reachability::ReachableAspects reachable_aspects;
  auto scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.build_cfg(); });
  auto reachable_objects = reachability::compute_reachable_objects(
      stores, ig_sets, &num_ignore_check_strings, &reachable_aspects);
  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });

  auto is_callable_instance_method = [&](const auto& s) {
    return reachable_aspects.callable_instance_methods.count(
        DexMethod::get_method(s)->as_def());
  };
  auto is_zombie_implementaton_method = [&](const auto& s) {
    return reachable_aspects.zombie_implementation_methods.count(
        DexMethod::get_method(s)->as_def());
  };
  EXPECT_FALSE(is_callable_instance_method("LK;.foo:()V"));
  EXPECT_TRUE(is_callable_instance_method("LKImpl1Derived;.foo:()V"));
  EXPECT_TRUE(is_zombie_implementaton_method(
      "LKImpl2;.foo:()V")); // not a target of any invoke, but need to keep
                            // method as class is instantiable and we need an
                            // implementation
}
