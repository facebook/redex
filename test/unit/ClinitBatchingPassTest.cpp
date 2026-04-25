/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ClinitBatchingPass.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DexClass.h"
#include "PassManager.h"
#include "RedexTest.h"

class ClinitBatchingPassTest : public RedexTest {
 protected:
  DexStoresVector stores;

  void SetUp() override {
    DexStore root_store("classes");

    // Create orchestrator class with @GenerateStaticInitBatch annotation.
    // eval_pass requires this annotation to find the orchestrator method.
    auto* anno_type = DexType::make_type(
        "Lcom/facebook/redex/annotations/GenerateStaticInitBatch;");
    ClassCreator anno_creator(anno_type);
    anno_creator.set_access(ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT |
                            ACC_ANNOTATION);
    anno_creator.set_super(DexType::make_type("Ljava/lang/Object;"));
    anno_creator.create();

    auto* orch_type =
        DexType::make_type("Lcom/facebook/test/ClinitBatchingOrchestrator;");
    ClassCreator orch_creator(orch_type);
    orch_creator.set_access(ACC_PUBLIC);
    orch_creator.set_super(DexType::make_type("Ljava/lang/Object;"));

    // Create initAllStatics() method with @GenerateStaticInitBatch annotation
    auto* method = dynamic_cast<DexMethod*>(DexMethod::make_method(
        orch_type, DexString::make_string("initAllStatics"),
        DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}))));
    always_assert(method != nullptr);
    method->set_access(ACC_PUBLIC | ACC_STATIC);

    // Attach annotation before make_concrete, because attach_annotation_set
    // rejects concrete non-synthetic methods.
    auto anno_set = std::make_unique<DexAnnotationSet>();
    anno_set->add_annotation(std::make_unique<DexAnnotation>(
        anno_type, DexAnnotationVisibility::DAV_RUNTIME));
    always_assert(method->attach_annotation_set(std::move(anno_set)));

    method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    // set_code after make_concrete, since make_concrete clears the code.
    method->set_code(assembler::ircode_from_string("((return-void))"));

    orch_creator.add_method(method);
    auto* orch_cls = orch_creator.create();

    root_store.add_classes({orch_cls});
    stores = {root_store};
  }

  void run_pass(const Json::Value& config = Json::nullValue) {
    ConfigFiles conf(config);
    conf.parse_global_config();
    ClinitBatchingPass pass;
    std::vector<Pass*> passes{&pass};
    PassManager manager(passes, conf);
    manager.run_passes(stores, conf);
  }
};

TEST_F(ClinitBatchingPassTest, test_skeleton_runs_without_crashing) {
  // Verify the pass skeleton can be instantiated and run without crashing.
  // This is a basic sanity test for the pass skeleton.
  run_pass();
}

TEST_F(ClinitBatchingPassTest, test_config_binding) {
  // Verify that config values are properly bound and the pass runs
  // successfully with a custom interaction_pattern.
  Json::Value config(Json::objectValue);
  config["ClinitBatchingPass"]["interaction_pattern"] = "ColdStart";

  // The pass emits interaction_pattern_set=1 when the pattern is non-empty.
  // We can't easily read metrics post-run (PassInfo is non-copyable and
  // get_metric requires m_current_pass_info), so we verify the config is
  // accepted without crashing.
  run_pass(config);
}
