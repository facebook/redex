/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "RedexTest.h"
#include "ResolveRefsPass.h"

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
