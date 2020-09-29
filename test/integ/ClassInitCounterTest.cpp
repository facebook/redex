/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "RedexTest.h"

namespace {

class ClassInitCounterTest : public RedexIntegrationTest {
 protected:
  DexClass* m_Foo =
      type_class(DexType::get_type("Lcom/facebook/redextest/classinit/Foo;"));
  DexClass* m_Bar =
      type_class(DexType::get_type("Lcom/facebook/redextest/classinit/Bar;"));
  DexClass* m_Baz =
      type_class(DexType::get_type("Lcom/facebook/redextest/classinit/Baz;"));
  DexClass* m_Qux =
      type_class(DexType::get_type("Lcom/facebook/redextest/classinit/Qux;"));
};

TEST_F(ClassInitCounterTest, Fixtures) {
  ASSERT_NE(nullptr, m_Foo);

  ASSERT_NE(nullptr, m_Bar);
  ASSERT_EQ(m_Foo->get_type(), m_Bar->get_super_class());

  ASSERT_NE(nullptr, m_Baz);
  ASSERT_EQ(m_Foo->get_type(), m_Baz->get_super_class());

  ASSERT_NE(nullptr, m_Qux);
  ASSERT_NE(m_Foo->get_type(), m_Qux->get_super_class());
}

} // namespace
