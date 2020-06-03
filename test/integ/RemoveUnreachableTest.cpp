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
#include "ProguardConfiguration.h"
#include "ProguardMatcher.h"
#include "ProguardParser.h"
#include "ReachableClasses.h"
#include "RedexTest.h"

#include "RemoveUnreachable.h"

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

class RemoveUnreachableTest : public RedexIntegrationTest {};

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

TEST_F(RemoveUnreachableTest, InheritanceTest) {
  // Make sure some unreachable things exist before we start.
  ASSERT_TRUE(find_vmethod(*classes, "LA;", "V", "bor", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LD;", "V", "bor", {}));

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

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 2);

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config));

  // Seed elements
  ASSERT_TRUE(find_class(*classes, "LRemoveUnreachableTest;"));
  ASSERT_TRUE(find_class(*classes, "LA;"));
  ASSERT_TRUE(find_ifield(*classes, "LA;", "I", "foo"));
  ASSERT_TRUE(find_vmethod(*classes, "LA;", "I", "bar", {}));

  // Elements transitively reachable via seeds.
  ASSERT_TRUE(find_vmethod(*classes, "LA;", "I", "baz", {}));

  // Elements not reachable via seeds.
  ASSERT_FALSE(find_vmethod(*classes, "LA;", "V", "bor", {}));

  // Subclass, used by testMethod
  ASSERT_TRUE(find_class(*classes, "LD;"));

  // Overrides of reachable elements
  ASSERT_TRUE(find_vmethod(*classes, "LD;", "I", "bar", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LD;", "I", "baz", {}));

  // Override of nonreachable elements.
  ASSERT_FALSE(find_vmethod(*classes, "LD;", "V", "bor", {}));

  // Class kept alive via array refrences
  ASSERT_TRUE(find_class(*classes, "LOnlyInArray;"));
  ASSERT_TRUE(find_ifield(*classes, "LA;", "[LOnlyInArray;", "arr"));
}

TEST_F(RemoveUnreachableTest, Inheritance2Test) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers class UseIt {
      void go(Child);
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config));

  // Still more inheritance trickiness.
  ASSERT_TRUE(find_class(*classes, "LParent;"));
  ASSERT_TRUE(find_class(*classes, "LChild;"));
  ASSERT_TRUE(find_vmethod(*classes, "LParent;", "V", "go", {}));
  ASSERT_FALSE(find_vmethod(*classes, "LChild;", "V", "go", {}));
}

TEST_F(RemoveUnreachableTest, Inheritance3Test) {
  auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keep class HoneyBadger
    -keepclasseswithmembers class BadgerTester {
      boolean testBadger(Badger);
    }
    -keep class HogBadger
    -keepclasseswithmembers class UseHasher {
      void test();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 4);

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config));

  // Another tricky inheritance case.
  ASSERT_TRUE(find_class(*classes, "LHoneyBadger;"));
  ASSERT_TRUE(find_vmethod(*classes, "LHoneyBadger;", "Z", "isAwesome", {}));
  // You might think that HogBadger.isAwesome() can be removed, since it
  // doesn't extend Badger.  But it's very tricky to remove this while still
  // getting the Guava Hasher case (below) correct.
  ASSERT_TRUE(find_vmethod(*classes, "LHogBadger;", "Z", "isAwesome", {}));

  // Inheritance case from Guava
  ASSERT_TRUE(find_class(*classes, "LHasher;"));
  ASSERT_TRUE(find_class(*classes, "LAbstractHasher;"));
  ASSERT_TRUE(find_class(*classes, "LTestHasher;"));
  ASSERT_TRUE(find_vmethod(*classes, "LHasher;", "V", "putBytes", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LTestHasher;", "V", "putBytes", {}));
}

TEST_F(RemoveUnreachableTest, InheritanceTriangleTest) {
  // I and Sub are both used within testMethod(), while Super is not
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testMethod();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config));
  // Weird inheritance triangle scenario.
  // I.wat() is kept
  // Sub implements I
  // Sub extends Super
  // Sub does not define wat(), but Super does
  // Super.wat() is a dex member that must be kept
  ASSERT_TRUE(find_class(*classes, "LI;"));
  ASSERT_TRUE(find_class(*classes, "LSuper;"));
  ASSERT_TRUE(find_vmethod(*classes, "LI;", "V", "wat", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LSuper;", "V", "wat", {}));
}
