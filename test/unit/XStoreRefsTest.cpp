/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional.hpp>
#include <gtest/gtest.h>

#include "Creators.h"
#include "DexClass.h"
#include "DexStore.h"
#include "RedexTest.h"
#include "SimpleClassHierarchy.h"

class XStoreRefsTest : public RedexTest {
  static DexClass* make_simple_class(const std::string& name) {
    ClassCreator cc(DexType::make_type(name.c_str()));
    cc.set_super(type::java_lang_Object());
    return cc.create();
  }

 public:
  void SetUp() override {
    auto helper = redex::test::SimpleClassHierarchy{};

    m_foo = helper.foo;
    m_bar = helper.bar;
    m_baz = helper.baz;
    m_qux = helper.qux;
    m_iquux = helper.iquux;
    m_quuz = helper.quuz;
    m_xyzzy = helper.xyzzy;

    m_store0_cls = make_simple_class("LStore0Cls;");
    m_store1_cls = make_simple_class("LStore1Cls;");
    m_store2_cls = make_simple_class("LStore2Cls;");

    auto store0 = DexStore("classes");
    store0.add_classes({m_foo, m_bar, m_qux, m_quuz, m_store0_cls});

    DexMetadata store1_metadata;
    store1_metadata.set_id("some_store");
    store1_metadata.set_dependencies({"dex"});
    auto store1 = DexStore(store1_metadata);
    store1.add_classes({m_baz, m_iquux, m_xyzzy, m_store1_cls});

    DexMetadata store2_metadata;
    store2_metadata.set_id("some_store2");
    store2_metadata.set_dependencies({"some_store"});
    auto store2 = DexStore(store2_metadata);
    store2.add_classes({m_store2_cls});

    stores = DexStoresVector{store0, store1, store2};
  }

 protected:
  // Will be created in SetUp. Hierarchy is:
  //
  // Object -> Throwable -> Foo (S0) -> Bar (S0) -> Baz (S1) -> Qux (S0)
  //                          |
  //          IQuux (S1) -> Quuz (S0)
  //
  // (Using Throwable for shortcut)

  DexClass* m_foo = nullptr;
  DexClass* m_bar = nullptr;
  DexClass* m_baz = nullptr;
  DexClass* m_qux = nullptr;
  DexClass* m_iquux = nullptr;
  DexClass* m_quuz = nullptr;
  DexClass* m_xyzzy = nullptr;

  DexClass* m_store0_cls = nullptr;
  DexClass* m_store1_cls = nullptr;
  DexClass* m_store2_cls = nullptr;

  DexStoresVector stores;
};

TEST_F(XStoreRefsTest, illegal_ref) {
  XStoreRefs xstores(stores);

  EXPECT_FALSE(xstores.illegal_ref(m_foo->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_foo->get_type(), m_bar->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_foo->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_foo->get_type(), m_qux->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_foo->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_foo->get_type(), m_quuz->get_type()));

  EXPECT_FALSE(xstores.illegal_ref(m_bar->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_bar->get_type(), m_bar->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_bar->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_bar->get_type(), m_qux->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_bar->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_bar->get_type(), m_quuz->get_type()));

  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_bar->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_qux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_baz->get_type(), m_quuz->get_type()));

  EXPECT_FALSE(xstores.illegal_ref(m_qux->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_qux->get_type(), m_bar->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_qux->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_qux->get_type(), m_qux->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_qux->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_qux->get_type(), m_quuz->get_type()));

  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_bar->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_qux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_iquux->get_type(), m_quuz->get_type()));

  EXPECT_FALSE(xstores.illegal_ref(m_quuz->get_type(), m_foo->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_quuz->get_type(), m_bar->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_quuz->get_type(), m_baz->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_quuz->get_type(), m_qux->get_type()));
  EXPECT_TRUE(xstores.illegal_ref(m_quuz->get_type(), m_iquux->get_type()));
  EXPECT_FALSE(xstores.illegal_ref(m_quuz->get_type(), m_quuz->get_type()));

  auto store0_type = m_store0_cls->get_type();
  auto store1_type = m_store1_cls->get_type();
  auto store2_type = m_store2_cls->get_type();
  EXPECT_FALSE(xstores.illegal_ref(store0_type, store0_type));
  EXPECT_TRUE(xstores.illegal_ref(store0_type, store1_type));
  EXPECT_TRUE(xstores.illegal_ref(store0_type, store2_type));
  EXPECT_FALSE(xstores.illegal_ref(store1_type, store0_type));
  EXPECT_FALSE(xstores.illegal_ref(store1_type, store1_type));
  EXPECT_TRUE(xstores.illegal_ref(store1_type, store2_type));
  EXPECT_FALSE(xstores.illegal_ref(store2_type, store0_type));
  EXPECT_FALSE(xstores.illegal_ref(store2_type, store1_type));
  EXPECT_FALSE(xstores.illegal_ref(store2_type, store2_type));
}

TEST_F(XStoreRefsTest, illegal_ref_load_types) {
  XStoreRefs xstores(stores);

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_foo->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_foo->get_type(), m_bar));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_foo->get_type(), m_baz));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_foo->get_type(), m_qux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_foo->get_type(), m_iquux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_foo->get_type(), m_quuz));

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_bar->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_bar->get_type(), m_bar));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_bar->get_type(), m_baz));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_bar->get_type(), m_qux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_bar->get_type(), m_iquux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_bar->get_type(), m_quuz));

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_bar));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_baz));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_qux));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_iquux));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_baz->get_type(), m_quuz));

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_qux->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_qux->get_type(), m_bar));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_qux->get_type(), m_baz));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_qux->get_type(), m_qux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_qux->get_type(), m_iquux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_qux->get_type(), m_quuz));

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_bar));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_baz));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_qux));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_iquux));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_iquux->get_type(), m_quuz));

  EXPECT_FALSE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_foo));
  EXPECT_FALSE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_bar));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_baz));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_qux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_iquux));
  EXPECT_TRUE(xstores.illegal_ref_load_types(m_quuz->get_type(), m_quuz));
}

TEST_F(XStoreRefsTest, transitive_resolved_dependencies) {
  XStoreRefs xstores(stores);
  auto store0 = &stores.at(0);
  const auto& store0_deps =
      xstores.get_transitive_resolved_dependencies(store0);
  EXPECT_EQ(store0_deps.size(), 0);

  auto store1 = &stores.at(1);
  const auto& store1_deps =
      xstores.get_transitive_resolved_dependencies(store1);
  EXPECT_EQ(store1_deps.size(), 1);
  EXPECT_TRUE(store1_deps.count(store0));

  auto store2 = &stores.at(2);
  const auto& store2_deps =
      xstores.get_transitive_resolved_dependencies(store2);
  EXPECT_EQ(store2_deps.size(), 2);
  EXPECT_TRUE(store2_deps.count(store0));
  EXPECT_TRUE(store2_deps.count(store1));
}
