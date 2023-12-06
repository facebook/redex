/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Reachability.h"
#include "RedexTest.h"
#include "Show.h"
#include "Walkers.h"

class VirtualTargetsReachabilityTest : public RedexIntegrationTest {};

TEST_F(VirtualTargetsReachabilityTest, invoke_super_subtlety) {
  // In this test, the method referenced in an invoke-super instruction is never
  // invoked.
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class VirtualTargetsReachabilityTest {
      public void root();
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
      stores, ig_sets, &num_ignore_check_strings, &reachable_aspects, false,
      /* relaxed_keep_class_members */ true,
      /* relaxed_keep_interfaces */ false,
      /* cfg_gathering_check_instantiable */ true);

  //// callable_instance_methods
  EXPECT_EQ(reachable_aspects.callable_instance_methods.size(), 5);
  auto is_callable_instance_method = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.callable_instance_methods.begin(),
                       reachable_aspects.callable_instance_methods.end(),
                       [s](const auto* method) { return show(method) == s; });
  };
  EXPECT_TRUE(is_callable_instance_method("LBase;.<init>:()V"));
  EXPECT_TRUE(is_callable_instance_method("LBase;.foo:()Ljava/lang/Object;"));
  EXPECT_TRUE(is_callable_instance_method("LSub;.<init>:()V"));
  EXPECT_FALSE(is_callable_instance_method("LSub;.foo:()Ljava/lang/Object;"));
  EXPECT_TRUE(is_callable_instance_method("LSub;.bar:()Ljava/lang/Object;"));
  EXPECT_TRUE(is_callable_instance_method(
      "LVirtualTargetsReachabilityTest;.<init>:()V"));

  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
}
