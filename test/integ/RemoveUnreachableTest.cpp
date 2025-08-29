/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <istream>
#include <memory>
#include <string>

#include <json/value.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "ReachableClasses.h"
#include "RedexTest.h"

#include "GlobalTypeAnalysisPass.h"
#include "RemoveUnreachable.h"
#include "Show.h"
#include "TypeAnalysisAwareRemoveUnreachable.h"
#include "VirtualScope.h"

class RemoveUnreachableTest : public RedexIntegrationTest {
  void SetUp() override {
    virt_scope::get_vmethods(type::java_lang_Object());
    auto* cls = type_class(type::java_lang_Object());
    // To make the assertion in reachability analysis happy
    cls->set_external();
  }
};

TEST_F(RemoveUnreachableTest, RelaxedInit) {
  // Regression test that makes sure that we identify that types are
  // instantiable when an instance is created with new-instance, even if the
  // invoked constructor got "relaxed".

  // Check that later unreachable methods are initially present
  ASSERT_TRUE(find_dmethod(*classes, "LE;", "V", "<init>", {"I"}));
  ASSERT_TRUE(find_dmethod(*classes, "LE;", "V", "bar", {}));

  // Rewrite constructor reference when creating instance of E,
  // "relaxing" it by rewriting to the base class.
  auto* test_method = find_dmethod(
      *classes, "LRemoveUnreachableTest;", "V", "testRelaxedInit", {});
  ASSERT_TRUE(test_method);
  auto* init_method = find_dmethod(*classes, "LE;", "V", "<init>", {"I"});
  ASSERT_TRUE(init_method);
  auto* base_init_method =
      find_dmethod(*classes, "LEBase;", "V", "<init>", {"I"});
  ASSERT_TRUE(init_method);

  size_t rewritten = 0;
  for (auto& mie : InstructionIterable(*test_method->get_code())) {
    if (mie.insn->opcode() == OPCODE_INVOKE_DIRECT &&
        mie.insn->get_method() == init_method) {
      mie.insn->set_method(base_init_method);
      rewritten++;
    }
  }
  ASSERT_EQ(rewritten, 1);
  type_class(init_method->get_class())->remove_method(init_method);

  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testRelaxedInit();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  RedexOptions options{};
  options.min_sdk = 22; // so that IRTypeChecker allows for relaxed inits
  run_passes({new RemoveUnreachablePass()},
             std::move(pg_config),
             Json::nullValue,
             options);

  // Seed elements
  ASSERT_TRUE(find_class(*classes, "LRemoveUnreachableTest;"));
  ASSERT_TRUE(find_dmethod(
      *classes, "LRemoveUnreachableTest;", "V", "testRelaxedInit", {}));

  // Elements transitively reachable via seeds.
  ASSERT_TRUE(find_class(*classes, "LE;"));
  ASSERT_FALSE(find_dmethod(*classes, "LE;", "V", "<init>", {"I"}));
  ASSERT_TRUE(find_vmethod(*classes, "LE;", "V", "foo", {}));
  ASSERT_TRUE(find_dmethod(*classes, "LE;", "V", "bar", {}));
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
  ASSERT_FALSE(find_dmethod(*classes, "LHoneyBadger;", "V", "<init>", {"Z"}));
  ASSERT_FALSE(find_vmethod(*classes, "LHoneyBadger;", "Z", "isAwesome", {}));
  ASSERT_TRUE(
      find_dmethod(*classes, "LHoneyBadgerInstantiated;", "V", "<init>", {}));
  ASSERT_TRUE(find_vmethod(
      *classes, "LHoneyBadgerInstantiated;", "Z", "isAwesome", {}));
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

TEST_F(RemoveUnreachableTest, TypeAnalysisInheritanceTest) {
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

  run_passes({new GlobalTypeAnalysisPass(),
              new TypeAnalysisAwareRemoveUnreachablePass()},
             std::move(pg_config));

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

TEST_F(RemoveUnreachableTest, TypeAnalysisInheritance2Test) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers class UseIt {
      void go(Child);
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({new GlobalTypeAnalysisPass(),
              new TypeAnalysisAwareRemoveUnreachablePass()},
             std::move(pg_config));

  // Still more inheritance trickiness.
  ASSERT_TRUE(find_class(*classes, "LParent;"));
  ASSERT_TRUE(find_class(*classes, "LChild;"));
  ASSERT_TRUE(find_vmethod(*classes, "LParent;", "V", "go", {}));
  ASSERT_FALSE(find_vmethod(*classes, "LChild;", "V", "go", {}));
}

TEST_F(RemoveUnreachableTest, TypeAnalysisInheritance3Test) {
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

  run_passes({new GlobalTypeAnalysisPass(),
              new TypeAnalysisAwareRemoveUnreachablePass()},
             std::move(pg_config));

  // Another tricky inheritance case.
  ASSERT_TRUE(find_class(*classes, "LHoneyBadger;"));
  ASSERT_FALSE(find_dmethod(*classes, "LHoneyBadger;", "V", "<init>", {"Z"}));
  ASSERT_FALSE(find_vmethod(*classes, "LHoneyBadger;", "Z", "isAwesome", {}));
  ASSERT_TRUE(
      find_dmethod(*classes, "LHoneyBadgerInstantiated;", "V", "<init>", {}));
  ASSERT_TRUE(find_vmethod(
      *classes, "LHoneyBadgerInstantiated;", "Z", "isAwesome", {}));
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

TEST_F(RemoveUnreachableTest, TypeAnalysisInheritanceTriangleTest) {
  // I and Sub are both used within testMethod(), while Super is not
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void testMethod();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({new GlobalTypeAnalysisPass(),
              new TypeAnalysisAwareRemoveUnreachablePass()},
             std::move(pg_config));
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

TEST_F(RemoveUnreachableTest, StaticInitializerTest) {
  // Make sure some things exist before we start.
  DexClass* a = find_class(*classes, "LA;");
  ASSERT_TRUE(a);
  DexClass* d = find_class(*classes, "LD;");
  ASSERT_TRUE(d);
  ASSERT_TRUE(a->get_clinit());
  ASSERT_TRUE(d->get_clinit());

  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keep class A
    -keep class D
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 2);

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config));

  // Seed elements
  a = find_class(*classes, "LA;");
  ASSERT_TRUE(a);
  d = find_class(*classes, "LD;");
  ASSERT_TRUE(d);
  ASSERT_FALSE(a->get_clinit());
  ASSERT_TRUE(d->get_clinit());
}

TEST_F(RemoveUnreachableTest, UnreferencedInterfaces) {
  // Make sure some things exist before we start.
  auto* cls = find_class(*classes, "LClassImplementingUnreferencedInterface;");
  ASSERT_TRUE(cls);
  const auto* interfaces = cls->get_interfaces();
  ASSERT_EQ(show(interfaces), "LUnreferencedInterface;");

  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class RemoveUnreachableTest {
      public void unreferencedInterface();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  Json::Value config(Json::objectValue);
  config["redex"] = Json::objectValue;
  config["redex"]["passes"] = Json::arrayValue;
  config["redex"]["passes"].append("RemoveUnreachablePass");
  config["RemoveUnreachablePass"] = Json::objectValue;
  config["RemoveUnreachablePass"]["prune_unreferenced_interfaces"] = true;

  run_passes({new RemoveUnreachablePass()}, std::move(pg_config), config);

  ASSERT_TRUE(find_class(*classes, "LClassImplementingUnreferencedInterface;"));
  ASSERT_TRUE(find_class(*classes, "LReferencedInterface;"));
  ASSERT_FALSE(find_class(*classes, "LUnreferencedInterface;"));
  interfaces = cls->get_interfaces();
  ASSERT_EQ(show(interfaces), "LReferencedInterface;");
}
