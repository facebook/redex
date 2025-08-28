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
#include <unistd.h>

#include <json/json.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "ReachableClasses.h"
#include "RedexTest.h"

#include "RemoveUnreachable.h"
#include "TypeAnalysisAwareRemoveUnreachable.h"
#include "VirtualScope.h"

class TypeAnalysisRemoveUnreachableTest : public RedexIntegrationTest {
  void SetUp() override {
    virt_scope::get_vmethods(type::java_lang_Object());
    auto* cls = type_class(type::java_lang_Object());
    // To make the assertion in reachability analysis happy
    cls->set_external();
  }
};

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

TEST_F(TypeAnalysisRemoveUnreachableTest, TypeAnalysisRMUTest3) {
  // Just because an instance of a class is being created, doesn't mean that all
  // of its methods must become vmethod targets; this is due the ability to
  // track exact vmethod targets
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class TypeAnalysisRemoveUnreachableTest {
      public void typeAnalysisRMUTest3();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({{new GlobalTypeAnalysisPass(),
               new TypeAnalysisAwareRemoveUnreachablePass()}},
             std::move(pg_config));
  ASSERT_TRUE(find_class(*classes, "LBase1;"));
  ASSERT_TRUE(find_class(*classes, "LSub1;"));
  ASSERT_TRUE(find_class(*classes, "LSubSub1;"));
  ASSERT_TRUE(find_vmethod(*classes, "LBase1;", "I", "foo", {}));
  ASSERT_TRUE(find_vmethod(*classes, "LSub1;", "I", "foo", {}));
  ASSERT_FALSE(find_vmethod(*classes, "LSubSub1;", "I", "foo", {}));
}

TEST_F(TypeAnalysisRemoveUnreachableTest, TypeAnalysisRMUTest4) {
  // We need to make sure that all directly instantiable classes somehow
  // override all inherited abstract methods.
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class TypeAnalysisRemoveUnreachableTest {
      public void typeAnalysisRMUTest4();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  run_passes({{new GlobalTypeAnalysisPass(),
               new TypeAnalysisAwareRemoveUnreachablePass()}},
             std::move(pg_config));
  ASSERT_TRUE(find_class(*classes, "LBase4;"));
  auto* intermediate_cls = find_class(*classes, "LIntermediate4;");
  ASSERT_TRUE(intermediate_cls);
  ASSERT_TRUE(find_class(*classes, "LSub4;"));
  ASSERT_TRUE(find_vmethod(*classes, "LBase4;", "V", "foo", {}));
  auto* intermediate_foo =
      find_vmethod(*classes, "LIntermediate4;", "V", "foo", {});
  ASSERT_TRUE(intermediate_foo);
  ASSERT_TRUE(find_vmethod(*classes, "LSub4;", "V", "foo", {}));
  ASSERT_TRUE(!is_abstract(intermediate_cls));
  ASSERT_TRUE(!is_abstract(intermediate_foo));
}

TEST_F(TypeAnalysisRemoveUnreachableTest, TypeAnalysisRMUTest5) {
  const auto& dexen = stores[0].get_dexen();
  auto pg_config = process_and_get_proguard_config(dexen, R"(
    -keepclasseswithmembers public class TypeAnalysisRemoveUnreachableTest {
      public void typeAnalysisRMUTest5();
    }
  )");

  ASSERT_TRUE(pg_config->ok);
  ASSERT_EQ(pg_config->keep_rules.size(), 1);

  Json::Value config(Json::objectValue);
  config["redex"] = Json::objectValue;
  config["redex"]["passes"] = Json::arrayValue;
  config["redex"]["passes"].append("GlobalTypeAnalysisPass");
  config["redex"]["passes"].append("TypeAnalysisAwareRemoveUnreachablePass");
  config["GlobalTypeAnalysisPass"] = Json::objectValue;
  config["TypeAnalysisAwareRemoveUnreachablePass"] = Json::objectValue;
  config["TypeAnalysisAwareRemoveUnreachablePass"]
        ["prune_uncallable_instance_method_bodies"] = true;
  config["TypeAnalysisAwareRemoveUnreachablePass"]
        ["prune_uninstantiable_insns"] = true;

  run_passes({{new GlobalTypeAnalysisPass(),
               new TypeAnalysisAwareRemoveUnreachablePass()}},
             std::move(pg_config), config);
  ASSERT_TRUE(find_class(*classes, "LBase5;"));
  ASSERT_TRUE(find_class(*classes, "LSub5;"));
  ASSERT_TRUE(find_vmethod(*classes, "LSub5;", "V", "foo", {}));
  auto* base_foo = find_vmethod(*classes, "LBase5;", "V", "foo", {});
  ASSERT_TRUE(base_foo);
  auto ii = InstructionIterable(base_foo->get_code());
  ASSERT_TRUE(std::any_of(ii.begin(), ii.end(), [](const MethodItemEntry& mie) {
    return opcode::is_throw(mie.insn->opcode());
  }));
  ASSERT_FALSE(find_class(*classes, "LDead;"));
}
