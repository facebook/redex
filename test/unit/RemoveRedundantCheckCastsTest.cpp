/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveRedundantCheckCasts.h"

#include <gtest/gtest.h>
#include <iterator>
#include <utility>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

namespace {

using namespace check_casts;

void run_passes(const std::vector<Pass*>& passes,
                std::vector<DexClass*> classes) {
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore store(dm);
  store.add_classes(std::move(classes));
  stores.emplace_back(std::move(store));
  PassManager manager(passes);
  manager.set_testing_mode();

  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_config(conf_obj);
  manager.run_passes(stores, dummy_config);
}

struct RemoveRedundantCheckCastsTest : public RedexTest {
  std::vector<DexClass*> m_classes;
  DexClass* m_class;
  DexTypeList* m_args;
  DexProto* m_proto;
  DexClass* m_cls;

  /**
   * Hierarchy:
   *
   * A extends B extends C
   * A implements I_A
   * B implements I_B0 and I_B1
   * C implements I_C
   */
  void create_hierarchy() {
    std::vector<DexType*> no_interfaces;

    DexType* i_c_type = DexType::make_type("I_C;");
    DexClass* i_c_cls = create_class(i_c_type, type::java_lang_Object(),
                                     no_interfaces, ACC_PUBLIC | ACC_INTERFACE);
    m_classes.push_back(i_c_cls);

    std::vector<DexType*> c_interfaces{i_c_type};
    DexType* c_type = DexType::make_type("C;");
    DexClass* c_cls = create_class(c_type, type::java_lang_Object(),
                                   c_interfaces, ACC_PUBLIC);
    m_classes.push_back(c_cls);

    DexType* i_b0_type = DexType::make_type("I_B0;");
    DexClass* i_b0_cls =
        create_class(i_b0_type, type::java_lang_Object(), no_interfaces,
                     ACC_PUBLIC | ACC_INTERFACE);
    m_classes.push_back(i_b0_cls);

    DexType* i_b1_type = DexType::make_type("I_B1;");
    DexClass* i_b1_cls =
        create_class(i_b1_type, type::java_lang_Object(), no_interfaces,
                     ACC_PUBLIC | ACC_INTERFACE);
    m_classes.push_back(i_b1_cls);

    std::vector<DexType*> b_interfaces{i_b0_type, i_b1_type};
    DexType* b_type = DexType::make_type("B;");
    DexClass* b_cls = create_class(b_type, c_type, b_interfaces, ACC_PUBLIC);
    m_classes.push_back(b_cls);

    DexType* i_a_type = DexType::make_type("I_A;");
    DexClass* i_a_cls = create_class(i_a_type, type::java_lang_Object(),
                                     no_interfaces, ACC_PUBLIC | ACC_INTERFACE);
    m_classes.push_back(i_a_cls);

    std::vector<DexType*> a_interfaces{i_a_type};
    DexType* a_type = DexType::make_type("A;");
    DexClass* a_cls = create_class(a_type, b_type, a_interfaces, ACC_PUBLIC);
    m_classes.push_back(a_cls);
  }

  void add_testing_class() {
    std::vector<DexType*> no_interfaces;
    DexType* type = DexType::make_type("testClass");
    m_cls =
        create_class(type, type::java_lang_Object(), no_interfaces, ACC_PUBLIC);
    m_classes.push_back(m_cls);
  }

  RemoveRedundantCheckCastsTest() {
    create_hierarchy();
    add_testing_class();

    m_args = DexTypeList::make_type_list({});
    m_proto = DexProto::make_proto(type::_void(), m_args);
  }

  DexMethod* create_empty_method(const std::string& name) {
    DexMethod* method =
        DexMethod::make_method(m_cls->get_type(), DexString::make_string(name),
                               m_proto)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(std::make_unique<IRCode>(method, 1));
    m_cls->add_method(method);
    return method;
  }

  void run_remove_redundant_check_casts() {
    std::vector<Pass*> passes = {new RemoveRedundantCheckCastsPass()};
    run_passes(passes, m_classes);
  }

  ~RemoveRedundantCheckCastsTest() {}
};

} // namespace

TEST_F(RemoveRedundantCheckCastsTest, simplestCase) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("simplestCase");

  auto str = R"(
    (
      (new-instance "C;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "C;.<init>:()V")
      (check-cast v0 "C;")
      (move-result-pseudo-object v0)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "C;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "C;.<init>:()V")
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, castingZero) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("castingZero");

  auto str = R"(
    (
      (const v0 0)
      (check-cast v0 "C;")
      (move-result-pseudo-object v0)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (const v0 0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, parentCheckCast) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("parentCheckCast");

  auto str = R"(
    (
      (new-instance "A;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "A;.<init>:()V")
      (check-cast v0 "B;")
      (move-result-pseudo-object v0)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "A;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "A;.<init>:()V")
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, skipParentCheckCast) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("skipParentCheckCast");

  auto str = R"(
    (
      (new-instance "A;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "A;.<init>:()V")
      (check-cast v0 "C;")
      (move-result-pseudo-object v0)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "A;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "A;.<init>:()V")
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, subclassCheckCast) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("subclassCheckCast");

  auto str = R"(
    (
      (new-instance "C;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "C;.<init>:()V")
      (check-cast v0 "B;")
      (move-result-pseudo-object v0)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "C;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "C;.<init>:()V")
      (check-cast v0 "B;")
      (move-result-pseudo-object v0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, directInterfaceCheckCast_WithMove) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("directInterfaceCheckCast_WithMove");

  auto str = R"(
    (
      (new-instance "B;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "B;.<init>:()V")
      (check-cast v0 "I_B0;")
      (move-result-pseudo-object v1)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "B;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "B;.<init>:()V")
      (move-object v1 v0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, parentInterfaceCheckCast_WithMove) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("parentInterfaceCheckCast_WithMove");

  auto str = R"(
    (
      (new-instance "B;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "B;.<init>:()V")
      (check-cast v0 "I_C;")
      (move-result-pseudo-object v1)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (new-instance "B;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "B;.<init>:()V")
      (move-object v1 v0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, sameTypeInterfaceCheckCast) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("sameTypeInterfaceCheckCast");

  auto str = R"(
    (
      (const v1 0)
      (const v0 0)

      (if-eqz v0 :lb0)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")
      (goto :lb1)

      (:lb0)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")

      (:lb1)
      (check-cast v1 "I_C;")
      (move-result-pseudo-object v1)
      (return-void)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (const v1 0)
      (const v0 0)

      (if-eqz v0 :lb1)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")
      (:lb0)
      (return-void)
      (:lb1)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")
      (goto :lb0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(RemoveRedundantCheckCastsTest, differentTypeInterfaceCheckCast) {
  using namespace dex_asm;
  DexMethod* method = create_empty_method("differentTypeInterfaceCheckCast");

  auto str = R"(
    (
      (const v1 0)
      (const v0 0)

      (if-eqz v0 :lb0)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")
      (goto :lb1)

      (:lb0)
      (new-instance "A;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "A;.<init>:()V")

      (:lb1)
      (check-cast v1 "I_C;")
      (move-result-pseudo-object v1)
      (return-void)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_remove_redundant_check_casts();

  auto expected_str = R"(
    (
      (const v1 0)
      (const v0 0)

      (if-eqz v0 :lb1)
      (new-instance "B;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "B;.<init>:()V")
      (:lb0)
      (check-cast v1 "I_C;")
      (move-result-pseudo-object v1)
      (return-void)
      (:lb1)
      (new-instance "A;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "A;.<init>:()V")
      (goto :lb0)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}
