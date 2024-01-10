/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <json/value.h>

#include "JarLoader.h"
#include "RedexOptions.h"
#include "RedexTest.h"
#include "ResolveRefsPass.h"
#include "ScopedCFG.h"
#include "Show.h"

class ResolveRefsTest : public RedexIntegrationTest {

 public:
  DexClass* m_base_cls;
  DexClass* m_sub_cls;
  DexClass* m_i_cls;
  DexClass* m_c_cls;
  DexMethod* m_i_getval;
  DexMethod* m_c_getval;

  void SetUp() override {
    m_base_cls = type_class(DexType::get_type("Lcom/facebook/redextest/Base;"));
    always_assert(m_base_cls);
    m_sub_cls = type_class(DexType::get_type("Lcom/facebook/redextest/Sub;"));
    always_assert(m_sub_cls);
    m_i_cls = type_class(DexType::get_type("Lcom/facebook/redextest/I;"));
    always_assert(m_i_cls);
    m_c_cls = type_class(DexType::get_type("Lcom/facebook/redextest/C;"));
    always_assert(m_c_cls);

    m_i_getval = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "I;.getVal:()Lcom/facebook/redextest/Base;")
                     ->as_def();
    always_assert(m_i_getval);
    m_c_getval = DexMethod::get_method(
                     "Lcom/facebook/redextest/"
                     "C;.getVal:()Lcom/facebook/redextest/Base;")
                     ->as_def();
    always_assert(m_c_getval);
  }

  void split_stores(std::vector<DexStore>& stores) {
    auto& root_store = stores.at(0);
    auto& root_dex_classes = root_store.get_dexen().at(0);

    DexMetadata second_dex_metadata;
    second_dex_metadata.set_id("Secondary");
    DexStore second_store(second_dex_metadata);

    second_store.add_classes(std::vector<DexClass*>{m_sub_cls});
    second_store.add_classes(std::vector<DexClass*>{m_c_cls});
    stores.emplace_back(second_store);

    root_dex_classes.erase(
        std::find(root_dex_classes.begin(), root_dex_classes.end(), m_sub_cls));
    root_dex_classes.erase(
        std::find(root_dex_classes.begin(), root_dex_classes.end(), m_c_cls));
  }
};

TEST_F(ResolveRefsTest, test_rtype_specialized_with_no_cross_dexstore_refs) {
  auto rtype = m_i_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype);
  EXPECT_EQ(rtype, m_base_cls->get_type());

  rtype = m_c_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype);
  EXPECT_EQ(rtype, m_base_cls->get_type());

  std::vector<Pass*> passes = {
      new ResolveRefsPass(),
  };

  run_passes(passes);

  auto rtype_after = m_i_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype_after);
  EXPECT_EQ(rtype_after, m_sub_cls->get_type());

  rtype_after = m_c_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype_after);
  EXPECT_EQ(rtype_after, m_sub_cls->get_type());
}

TEST_F(ResolveRefsTest, test_rtype_not_specialized_with_cross_dexstore_refs) {
  auto rtype = m_i_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype);
  EXPECT_EQ(rtype, m_base_cls->get_type());

  rtype = m_c_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype);
  EXPECT_EQ(rtype, m_base_cls->get_type());

  split_stores(stores);

  std::vector<Pass*> passes = {
      new ResolveRefsPass(),
  };

  run_passes(passes);

  auto rtype_after = m_i_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype_after);
  EXPECT_EQ(rtype_after, m_base_cls->get_type());

  rtype_after = m_c_getval->get_proto()->get_rtype();
  ASSERT_TRUE(rtype_after);
  EXPECT_EQ(rtype_after, m_base_cls->get_type());
}

TEST_F(ResolveRefsTest, test_invoke_virtual_specialization_to_interface) {
  // Ensure that invoke-virtual on Object.toString() results in a correct opcode
  // when an interface also defines a toString() method. This test relies on jdk
  // classes (as it mimics a real world scenario), so manually suck them in to
  // make the code resolvable.
  auto& root_store = stores.at(0);
  std::string sdk_jar = android_sdk_jar_path();
  ASSERT_TRUE(load_jar_file(DexLocation::make_location("", sdk_jar)));

  ClassCreator foo_creator(DexType::make_type("LFoo;"));
  foo_creator.set_super(type::java_lang_Object());

  auto method =
      DexMethod::make_method("LFoo;.bar:()V")
          ->make_concrete(ACC_STATIC | ACC_PUBLIC, false /* is_virtual */);
  auto code_str = R"(
    (
      (sget-object "Landroid/os/Build;.BRAND:Ljava/lang/String;")
      (move-result-pseudo-object v0)
      (const v1 0)
      (const v2 1)
      (invoke-interface (v0 v1 v2) "Ljava/lang/CharSequence;.subSequence:(II)Ljava/lang/CharSequence;")
      (move-result-object v3)
      (invoke-virtual (v3) "Ljava/lang/Object;.toString:()Ljava/lang/String;")
      (move-result-object v4)
      (invoke-static (v4 v4) "Landroid/util/Log;.v:(Ljava/lang/String;Ljava/lang/String;)I")
      (return-void)
    )
  )";
  method->set_code(assembler::ircode_from_string(code_str));
  foo_creator.add_method(method);
  auto cls = foo_creator.create();
  method->get_code()->build_cfg();

  root_store.add_classes(std::vector<DexClass*>{cls});

  std::vector<Pass*> passes = {
      new ResolveRefsPass(),
  };

  RedexOptions options{};
  // A sensible lower bound for most of our apps. Need to kick on the resolving
  // of external refs for the above ir code to be relevant.
  options.min_sdk = 21;

  Json::Value root;
  auto val_cstr = std::getenv("api");
  redex_assert(val_cstr != nullptr);
  root["android_sdk_api_21_file"] = val_cstr;

  run_passes(passes, nullptr, root, options);

  cfg::ScopedCFG cfg(method->get_code());
  IRInstruction* invoke_to_string{nullptr};
  for (auto it = cfg::ConstInstructionIterator(*cfg, true); !it.is_end();
       ++it) {
    if (it->insn->has_method()) {
      auto method_name = it->insn->get_method()->get_name();
      if (method_name->str_copy() == "toString") {
        invoke_to_string = it->insn;
      }
    }
  }

  EXPECT_NE(invoke_to_string, nullptr)
      << "Relevant instruction to assert was not found!";
  EXPECT_EQ(invoke_to_string->opcode(), OPCODE_INVOKE_VIRTUAL)
      << "Incorrect invoke type!";
  EXPECT_EQ(invoke_to_string->get_method()->get_class(),
            type::java_lang_Object())
      << "Should not rebind toString! Got "
      << show(invoke_to_string->get_method()->get_class());
}
