/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Creators.h"
#include "DexClass.h"
#include "Pass.h"
#include "RedexTest.h"
#include <gtest/gtest.h>
#include <json/value.h>
#include <stdint.h>

class ExamplePartialPass : public PartialPass {
 public:
  bool true_after_bind = false;

  std::unordered_set<DexClass*> visited_classes;

  void bind_partial_pass_config() override {
    bind("true_after_bind", false, true_after_bind);
  }

  ExamplePartialPass() : PartialPass("ExamplePartialPass") {}

  void run_partial_pass(DexStoresVector& /* stores */,
                        Scope scope,
                        ConfigFiles& /* conf */,
                        PassManager& /* mgr */) override {
    // basically just visit classes
    for (DexClass* cls : scope) {
      visited_classes.emplace(cls);
    }
  }
};

static ExamplePartialPass s_pass;

class PartialPassTest : public RedexTest {
 protected:
  DexStore root_store;

  DexClass* class_out_of_package;
  DexClass* class_in_package;

  DexStoresVector stores;

  std::unique_ptr<ExamplePartialPass> pass;

 public:
  PartialPassTest() : root_store("classes") {}

  void SetUp() override { pass = std::make_unique<ExamplePartialPass>(); }

  void setup_stores() {
    // Effectively clear the root store
    root_store = DexStore("classes");

    auto class_out_of_package_type = DexType::make_type("LTopLevelClass;");
    ClassCreator class_out_of_package_creator(class_out_of_package_type);
    class_out_of_package_creator.set_access(ACC_PUBLIC | ACC_FINAL);
    class_out_of_package_creator.set_super(type::java_lang_Object());
    class_out_of_package = class_out_of_package_creator.create();

    auto class_in_package_type = DexType::make_type("Lcom/facebook/PkgClass;");
    ClassCreator class_in_package_creator(class_in_package_type);
    class_in_package_creator.set_access(ACC_PUBLIC | ACC_FINAL);
    class_in_package_creator.set_super(type::java_lang_Object());
    class_in_package = class_in_package_creator.create();
    DexClasses dex{class_out_of_package, class_in_package};
    root_store.add_classes(std::move(dex));

    stores = {root_store};
  }

  Json::Value build_config(bool run_on_package_only) {
    Json::Value config(Json::objectValue);
    config["redex"] = Json::objectValue;
    config["redex"]["passes"] = Json::arrayValue;
    config["redex"]["passes"].append("ExamplePartialPass");
    config["ExamplePartialPass"] = Json::objectValue;
    if (run_on_package_only) {
      config["ExamplePartialPass"]["run_on_packages"] = Json::arrayValue;
      config["ExamplePartialPass"]["run_on_packages"].append("Lcom/facebook/");
    }
    config["ExamplePartialPass"]["true_after_bind"] = true;
    return config;
  }

  void run_passes(const Json::Value& config) {
    setup_stores();
    ConfigFiles conf(config);
    std::vector<Pass*> passes{pass.get()};
    PassManager manager(passes, conf);
    manager.set_testing_mode();
    manager.run_passes(stores, conf);
  }
};

TEST_F(PartialPassTest, test_run_pass_in_select_package) {
  run_passes(build_config(/* run_on_package_only = */ true));
  EXPECT_TRUE(pass->true_after_bind);
  EXPECT_EQ(1, pass->visited_classes.size());
  EXPECT_TRUE(pass->visited_classes.count(class_in_package));
  EXPECT_FALSE(pass->visited_classes.count(class_out_of_package));
}

TEST_F(PartialPassTest, test_run_pass_on_all_classes) {
  run_passes(build_config(/* run_on_package_only = */ false));
  EXPECT_TRUE(pass->true_after_bind);
  EXPECT_EQ(2, pass->visited_classes.size());
  EXPECT_TRUE(pass->visited_classes.count(class_in_package));
  EXPECT_TRUE(pass->visited_classes.count(class_out_of_package));
}
