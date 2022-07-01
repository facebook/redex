/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "Creators.h"
#include "DexStore.h"
#include "RedexTest.h"

class DexStoreTest : public RedexTest {
 public:
  DexClass* create_class(const char* class_name) {
    ClassCreator cc(DexType::make_type(class_name));
    cc.set_super(type::java_lang_Object());
    return cc.create();
  }

  DexStoresVector construct_empty_stores() {
    DexStoresVector stores;
    DexStore root_store("classes");
    // Empty primary dex.
    root_store.add_classes({});
    stores.emplace_back(std::move(root_store));
    return stores;
  }

  void squash_and_check(DexStoresVector stores) {
    auto before_scope = build_class_scope(stores);
    squash_into_one_dex(stores);
    EXPECT_EQ(stores.size(), 1);
    EXPECT_EQ(stores[0].get_dexen().size(), 1);
    auto after_scope = build_class_scope(stores);
    EXPECT_THAT(before_scope, ::testing::ContainerEq(after_scope));
  }
};

TEST_F(DexStoreTest, squash_dexes) {
  auto stores = construct_empty_stores();
  squash_and_check(stores);

  // Add one class to primary dex.
  auto& dexes = stores[0].get_dexen();
  dexes[0].emplace_back(create_class("Ltype0;"));
  squash_and_check(stores);

  // Add a secondary dex.
  stores[0].add_classes({create_class("Lsecond0;")});
  squash_and_check(stores);

  // Add a non-root non_root_store.
  DexStore non_root_store("other");
  non_root_store.add_classes({create_class("Lother1;")});
  non_root_store.add_classes({create_class("Lother2;")});
  stores.emplace_back(std::move(non_root_store));
  squash_and_check(stores);
}
