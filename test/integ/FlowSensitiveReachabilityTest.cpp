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

class FlowSensitiveReachabilityTest : public RedexIntegrationTest {};

TEST_F(FlowSensitiveReachabilityTest,
       relaxed_keep_class_members_and_cfg_gathering_check_instantiable) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class FlowSensitiveReachabilityTest {
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
      /* cfg_gathering_check_instantiable */ true);

  //// instantiable_types
  EXPECT_EQ(reachable_aspects.instantiable_types.size(), 5);
  auto is_instantiable = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.instantiable_types.begin(),
                       reachable_aspects.instantiable_types.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_instantiable("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_instantiable("LData;"));
  EXPECT_FALSE(is_instantiable("LDataHolder;"));
  EXPECT_FALSE(is_instantiable("LLegacyInstantiable;"));
  EXPECT_TRUE(is_instantiable("LStringInstantiable;"));
  EXPECT_TRUE(is_instantiable("LBase;"));
  EXPECT_TRUE(is_instantiable("LIntermediate;"));
  EXPECT_TRUE(is_instantiable("LRegularInstantiable;"));

  //// dynamically_referenced_classes
  EXPECT_EQ(reachable_aspects.dynamically_referenced_classes.size(), 2);
  auto is_dynamically_referenced = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.dynamically_referenced_classes.begin(),
                       reachable_aspects.dynamically_referenced_classes.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_FALSE(is_dynamically_referenced("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_dynamically_referenced("LData;"));
  EXPECT_FALSE(is_dynamically_referenced("LDataHolder;"));
  EXPECT_FALSE(is_dynamically_referenced("LLegacyInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LStringInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LRegularInstantiable;"));

  //// instructions_unvisited
  EXPECT_GT(reachable_aspects.instructions_unvisited, 0);

  //// uninstantiable_dependencies
  EXPECT_EQ(reachable_aspects.uninstantiable_dependencies.size(), 1);
  auto is_uninstantiable_dependency = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.uninstantiable_dependencies.begin(),
                       reachable_aspects.uninstantiable_dependencies.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_uninstantiable_dependency("LDataHolder;"));

  // Code sweeping
  auto uninstantiables_stats = reachability::sweep_code(
      stores, /* prune_uncallable_instance_method_bodies */ false,
      /* skip_uncallable_virtual_methods */ false, reachable_aspects);
  EXPECT_EQ(uninstantiables_stats.field_accesses_on_uninstantiable, 3);
  EXPECT_EQ(uninstantiables_stats.invokes, 2);
  EXPECT_EQ(uninstantiables_stats.check_casts, 1);
  EXPECT_EQ(uninstantiables_stats.instance_ofs, 1);

  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
}

TEST_F(FlowSensitiveReachabilityTest, cfg_gathering_check_instance_callable) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class FlowSensitiveReachabilityTest {
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
      /* cfg_gathering_check_instantiable */ true,
      /* cfg_gathering_check_instance_callable */ true);

  //// instantiable_types
  EXPECT_EQ(reachable_aspects.instantiable_types.size(), 5);
  auto is_instantiable = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.instantiable_types.begin(),
                       reachable_aspects.instantiable_types.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_instantiable("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_instantiable("LData;"));
  EXPECT_FALSE(is_instantiable("LDataHolder;"));
  EXPECT_FALSE(is_instantiable("LLegacyInstantiable;"));
  EXPECT_TRUE(is_instantiable("LStringInstantiable;"));
  EXPECT_TRUE(is_instantiable("LBase;"));
  EXPECT_TRUE(is_instantiable("LIntermediate;"));
  EXPECT_TRUE(is_instantiable("LRegularInstantiable;"));

  //// dynamically_referenced_classes
  EXPECT_EQ(reachable_aspects.dynamically_referenced_classes.size(), 2);
  auto is_dynamically_referenced = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.dynamically_referenced_classes.begin(),
                       reachable_aspects.dynamically_referenced_classes.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_FALSE(is_dynamically_referenced("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_dynamically_referenced("LData;"));
  EXPECT_FALSE(is_dynamically_referenced("LDataHolder;"));
  EXPECT_FALSE(is_dynamically_referenced("LLegacyInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LStringInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LRegularInstantiable;"));

  //// instructions_unvisited
  EXPECT_GT(reachable_aspects.instructions_unvisited, 0);

  //// uninstantiable_dependencies
  EXPECT_EQ(reachable_aspects.uninstantiable_dependencies.size(), 1);
  auto is_uninstantiable_dependency = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.uninstantiable_dependencies.begin(),
                       reachable_aspects.uninstantiable_dependencies.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_uninstantiable_dependency("LDataHolder;"));

  // Code sweeping
  auto uninstantiables_stats = reachability::sweep_code(
      stores, /* prune_uncallable_instance_method_bodies */ true,
      /* skip_uncallable_virtual_methods */ false, reachable_aspects);
  EXPECT_EQ(uninstantiables_stats.field_accesses_on_uninstantiable, 1);
  EXPECT_EQ(uninstantiables_stats.invokes, 2);
  EXPECT_EQ(uninstantiables_stats.check_casts, 1);
  EXPECT_EQ(uninstantiables_stats.instance_ofs, 1);
  EXPECT_EQ(uninstantiables_stats.throw_null_methods, 6);

  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
}

TEST_F(FlowSensitiveReachabilityTest, sweep_uncallable_virtual_methods) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class FlowSensitiveReachabilityTest {
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
      /* cfg_gathering_check_instantiable */ true,
      /* cfg_gathering_check_instance_callable */ true);

  //// instantiable_types
  EXPECT_EQ(reachable_aspects.instantiable_types.size(), 5);
  auto is_instantiable = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.instantiable_types.begin(),
                       reachable_aspects.instantiable_types.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_instantiable("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_instantiable("LData;"));
  EXPECT_FALSE(is_instantiable("LDataHolder;"));
  EXPECT_FALSE(is_instantiable("LLegacyInstantiable;"));
  EXPECT_TRUE(is_instantiable("LStringInstantiable;"));
  EXPECT_TRUE(is_instantiable("LBase;"));
  EXPECT_TRUE(is_instantiable("LIntermediate;"));
  EXPECT_TRUE(is_instantiable("LRegularInstantiable;"));

  //// dynamically_referenced_classes
  EXPECT_EQ(reachable_aspects.dynamically_referenced_classes.size(), 2);
  auto is_dynamically_referenced = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.dynamically_referenced_classes.begin(),
                       reachable_aspects.dynamically_referenced_classes.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_FALSE(is_dynamically_referenced("LFlowSensitiveReachabilityTest;"));
  EXPECT_FALSE(is_dynamically_referenced("LData;"));
  EXPECT_FALSE(is_dynamically_referenced("LDataHolder;"));
  EXPECT_FALSE(is_dynamically_referenced("LLegacyInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LStringInstantiable;"));
  EXPECT_TRUE(is_dynamically_referenced("LRegularInstantiable;"));

  //// instructions_unvisited
  EXPECT_GT(reachable_aspects.instructions_unvisited, 0);

  //// uninstantiable_dependencies
  EXPECT_EQ(reachable_aspects.uninstantiable_dependencies.size(), 1);
  auto is_uninstantiable_dependency = [&](const std::string_view& s) {
    return std::any_of(reachable_aspects.uninstantiable_dependencies.begin(),
                       reachable_aspects.uninstantiable_dependencies.end(),
                       [s](const auto* cls) { return cls->str() == s; });
  };
  EXPECT_TRUE(is_uninstantiable_dependency("LDataHolder;"));

  // Code sweeping
  auto uninstantiables_stats = reachability::sweep_code(
      stores, /* prune_uncallable_instance_method_bodies */ true,
      /* skip_uncallable_virtual_methods */ true, reachable_aspects);
  EXPECT_EQ(uninstantiables_stats.field_accesses_on_uninstantiable, 1);
  EXPECT_EQ(uninstantiables_stats.invokes, 2);
  EXPECT_EQ(uninstantiables_stats.check_casts, 1);
  EXPECT_EQ(uninstantiables_stats.instance_ofs, 1);
  EXPECT_EQ(uninstantiables_stats.throw_null_methods, 3);

  reachability::sweep(stores, *reachable_objects);
  uninstantiables_stats =
      reachability::sweep_uncallable_virtual_methods(stores, reachable_aspects);
  EXPECT_EQ(uninstantiables_stats.abstracted_vmethods, 1);
  EXPECT_EQ(uninstantiables_stats.abstracted_classes, 1);
  EXPECT_EQ(uninstantiables_stats.removed_vmethods, 1);

  walk::parallel::code(scope, [&](auto*, auto& code) { code.clear_cfg(); });
}
