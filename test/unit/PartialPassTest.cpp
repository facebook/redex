/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Creators.h"
#include "DexClass.h"
#include "Pass.h"
#include "RedexTest.h"
#include <gtest/gtest.h>
#include <json/json.h>
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
                        Scope& scope,
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
  DexStore module_store;

  DexClass* class_in_root_store;
  DexClass* class_in_module;

  DexStoresVector stores;

  std::unique_ptr<ExamplePartialPass> pass;

 public:
  PartialPassTest() : root_store("classes"), module_store("module") {}

  void SetUp() override { pass = std::make_unique<ExamplePartialPass>(); }

  void setup_stores() {
    // Effectively clear the root store
    root_store = DexStore("classes");
    auto class_in_root_store_type = DexType::make_type("LClassInRootStore;");
    ClassCreator class_in_root_store_creator(class_in_root_store_type);
    class_in_root_store_creator.set_access(ACC_PUBLIC | ACC_FINAL);
    class_in_root_store_creator.set_super(type::java_lang_Object());
    class_in_root_store = class_in_root_store_creator.create();
    DexClasses dex_in_root_store{class_in_root_store};
    root_store.add_classes(std::move(dex_in_root_store));

    // Effectively clear the module store
    module_store = DexStore("module");
    auto class_in_module_type = DexType::make_type("LClassInModule;");
    ClassCreator class_in_module_creator(class_in_module_type);
    class_in_module_creator.set_access(ACC_PUBLIC | ACC_FINAL);
    class_in_module_creator.set_super(type::java_lang_Object());
    class_in_module = class_in_module_creator.create();
    DexClasses dex_in_module{class_in_module};
    module_store.add_classes(std::move(dex_in_module));

    stores = {root_store, module_store};
  }

  Json::Value build_config(bool run_on_module_only) {
    Json::Value config(Json::objectValue);
    config["redex"] = Json::objectValue;
    config["redex"]["passes"] = Json::arrayValue;
    config["redex"]["passes"].append("ExamplePartialPass");
    config["ExamplePartialPass"] = Json::objectValue;
    if (run_on_module_only) {
      config["ExamplePartialPass"]["run_on_stores"] = Json::arrayValue;
      config["ExamplePartialPass"]["run_on_stores"].append("module");
    }
    config["ExamplePartialPass"]["true_after_bind"] = true;
    return config;
  }

  void run_passes(const Json::Value& config) {
    setup_stores();
    ConfigFiles conf(config);
    std::vector<Pass*> passes{pass.get()};
    PassManager manager(passes, config);
    manager.set_testing_mode();
    manager.run_passes(stores, conf);
  }
};

TEST_F(PartialPassTest, test_run_pass_on_limited_stores) {
  run_passes(build_config(/* run_on_module_only = */ true));
  EXPECT_TRUE(pass->true_after_bind);
  EXPECT_EQ(1, pass->visited_classes.size());
  EXPECT_TRUE(pass->visited_classes.count(class_in_module));
  EXPECT_FALSE(pass->visited_classes.count(class_in_root_store));
}

TEST_F(PartialPassTest, test_run_pass_on_all_stores) {
  run_passes(build_config(/* run_on_module_only = */ false));
  EXPECT_TRUE(pass->true_after_bind);
  EXPECT_EQ(2, pass->visited_classes.size());
  EXPECT_TRUE(pass->visited_classes.count(class_in_module));
  EXPECT_TRUE(pass->visited_classes.count(class_in_root_store));
}
