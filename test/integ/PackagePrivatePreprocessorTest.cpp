/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PackagePrivatePreprocessor.h"

#include <boost/algorithm/string/join.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexLoader.h"
#include "JarLoader.h"
#include "RedexTest.h"
#include "Show.h"
#include "Walkers.h"

struct PackagePrivateProcessorTest : public RedexIntegrationTest {};

TEST_F(PackagePrivateProcessorTest, verify) {
  std::unordered_map<DexMethod*, const DexString*> vmethods;
  std::set<DexMethod*, dexmethods_comparator> ordered_vmethods;
  walk::methods(build_class_scope(stores), [&](DexMethod* method) {
    if (!method->is_virtual()) {
      return;
    }
    vmethods.emplace(method, method->get_name());
    ordered_vmethods.insert(method);
  });

  std::string sdk_jar = android_sdk_jar_path();
  ASSERT_TRUE(load_jar_file(DexLocation::make_location("", sdk_jar)));

  auto pass = std::make_unique<PackagePrivatePreprocessorPass>();
  std::vector<Pass*> passes = {pass.get()};

  run_passes(passes);

  std::string actual = "\n";
  for (auto* method : ordered_vmethods) {
    actual += show(method) + " <= " + show(vmethods.at(method)) + "\n";
  }

  std::string expected = R"(
LP/C;.interface_collision_not_okay:()V <= interface_collision_not_okay
LP/C;.package_private_collision1_okay:()V <= package_private_collision1_okay
LP/C;.package_private_collision2_okay:()V <= package_private_collision2_okay
LP/C;.package_private_collision3_not_okay:()V <= package_private_collision3_not_okay
LP/C;.package_private_collision3_okay:()V <= package_private_collision3_okay
LP/C;.simple_public_okay:()V <= simple_public_okay
LP/E;.package_private_collision3_not_okay:()V <= package_private_collision3_not_okay
LP/E;.package_private_collision3_okay:()V <= package_private_collision3_okay
LP/E;.simple_public_okay:()V <= simple_public_okay
LQ/D;.package_private_collision1_okay$REDEX$PPP$gBwUyFaWvqi:()V <= package_private_collision1_okay
LQ/D;.package_private_collision2_okay$REDEX$PPP$gBwUyFaWvqi:()V <= package_private_collision2_okay
LQ/D;.package_private_collision3_not_okay:()V <= package_private_collision3_not_okay
LQ/D;.package_private_collision3_okay$REDEX$PPP$gBwUyFaWvqi:()V <= package_private_collision3_okay
LQ/F;.interface_collision_not_okay:()V <= interface_collision_not_okay
LQ/F;.package_private_collision3_not_okay:()V <= package_private_collision3_not_okay
LQ/F;.package_private_collision3_okay$REDEX$PPP$gBwUyFaWvqi:()V <= package_private_collision3_okay
LQ/F;.simple_public_okay:()V <= simple_public_okay
LR/G;.package_private_collision3_okay$REDEX$PPP$c5q1BfR0O0k:()V <= package_private_collision3_okay
LR/G;.simple_public_okay:()V <= simple_public_okay
LR/I;.interface_collision_not_okay:()V <= interface_collision_not_okay
)";

  EXPECT_EQ(actual, expected);

  const auto& stats = pass->get_stats();

  EXPECT_EQ(stats.unresolved_types, 0);
  EXPECT_EQ(stats.external_inaccessible_types, 0);
  EXPECT_EQ(stats.internal_inaccessible_types, 0);

  EXPECT_EQ(stats.unresolved_fields, 0);
  EXPECT_EQ(stats.external_inaccessible_private_fields, 0);
  EXPECT_EQ(stats.external_inaccessible_fields, 0);
  EXPECT_EQ(stats.internal_inaccessible_fields, 0);

  EXPECT_EQ(stats.unresolved_methods, 0);
  EXPECT_EQ(stats.external_inaccessible_private_methods, 0);
  EXPECT_EQ(stats.external_inaccessible_methods, 0);
  EXPECT_EQ(stats.internal_inaccessible_methods, 0);

  EXPECT_EQ(stats.apparent_override_inaccessible_methods, 3);
  EXPECT_EQ(stats.override_package_private_methods, 3);

  EXPECT_EQ(stats.package_private_accessed_classes, 0);
  EXPECT_EQ(stats.package_private_accessed_methods, 0);
  EXPECT_EQ(stats.package_private_accessed_fields, 0);
  EXPECT_EQ(stats.new_virtual_scope_roots, 5);

  EXPECT_EQ(stats.renamed_methods, 5);
  EXPECT_EQ(stats.updated_method_refs, 0);
  EXPECT_EQ(stats.publicized_classes, 0);
  EXPECT_EQ(stats.publicized_methods, 6);
  EXPECT_EQ(stats.publicized_fields, 0);
  EXPECT_EQ(stats.unsupported_unrenamable_methods, 0);
  EXPECT_EQ(stats.unsupported_interface_implementations, 1);
  EXPECT_EQ(stats.unsupported_multiple_package_private_overrides, 1);
}
