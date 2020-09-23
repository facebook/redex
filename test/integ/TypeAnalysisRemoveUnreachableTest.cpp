/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <istream>
#include <memory>
#include <string>
#include <unistd.h>

#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "ReachableClasses.h"
#include "RedexTest.h"

#include "RemoveUnreachable.h"
#include "TypeAnalysisAwareRemoveUnreachable.h"

class TypeAnalysisRemoveUnreachableTest : public RedexIntegrationTest {};

TEST_F(TypeAnalysisRemoveUnreachableTest, TypeAnalysisRMUTest1) {
  // I and Sub are both used within testMethod(), while Super is not
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class TypeAnalysisRemoveUnreachableTest {
      public void typeAnalysisRMUTest1();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({{new GlobalTypeAnalysisPass(),
               new TypeAnalysisAwareRemoveUnreachablePass()}},
             std::move(pg_config));
  ASSERT_TRUE(find_class(*classes, "LBase1;"));
  ASSERT_TRUE(find_class(*classes, "LSub1;"));
  ASSERT_FALSE(find_class(*classes, "LSubSub1;"));
  ASSERT_TRUE(find_vmethod(*classes, "LBase1;", "I", "foo", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LSub1;", "I", "foo", {}));
}

TEST_F(TypeAnalysisRemoveUnreachableTest, TypeAnalysisRMUTest2) {
  // I and Sub are both used within testMethod(), while Super is not
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class TypeAnalysisRemoveUnreachableTest {
      public void typeAnalysisRMUTest2();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({{new GlobalTypeAnalysisPass(),
               new TypeAnalysisAwareRemoveUnreachablePass()}},
             std::move(pg_config));
  ASSERT_TRUE(find_class(*classes, "LIntf1;"));
  ASSERT_TRUE(find_class(*classes, "LImpl1;"));
  ASSERT_TRUE(find_class(*classes, "LImpl2;"));
  ASSERT_TRUE(find_vmethod(*classes, "LIntf1;", "I", "bar", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LImpl1;", "I", "bar", {}));
  ASSERT_FALSE(find_vmethod(*classes, "LImpl2;", "I", "bar", {}));
}
