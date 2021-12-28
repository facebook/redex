/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

    auto store0 = DexStore("store0");
    store0.add_classes({m_foo, m_bar, m_qux, m_quuz});
    auto store1 = DexStore("store1");
    store1.add_classes({m_baz, m_iquux, m_xyzzy});
    stores = DexStoresVector{store0, store1};
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
