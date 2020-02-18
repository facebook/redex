/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexTypeEnvironment.h"

#include <boost/optional/optional_io.hpp>

#include "Creators.h"
#include "RedexTest.h"

struct DexTypeEnvironmentTest : public RedexTest {
 public:
  /*
   *   A
   *  /
   * B
   */
  DexTypeEnvironmentTest() {
    m_type_a = DexType::make_type("A");
    ClassCreator creator(m_type_a);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_b = DexType::make_type("B");
    creator = ClassCreator(m_type_b);
    creator.set_super(m_type_a);
    creator.create();
  }

 protected:
  DexType* m_type_a;
  DexType* m_type_b;
};

TEST_F(DexTypeEnvironmentTest, BasicTest) {
  auto env = DexTypeEnvironment();
  EXPECT_TRUE(env.is_top());
  auto& reg_env = env.get_reg_environment();
  EXPECT_TRUE(reg_env.is_top());
  auto& field_env = env.get_field_environment();
  EXPECT_TRUE(field_env.is_top());
}

TEST_F(DexTypeEnvironmentTest, RegisterEnvTest) {
  auto env = DexTypeEnvironment();
  reg_t v0 = 0;
  auto type = env.get(v0);
  EXPECT_TRUE(type.is_top());

  env.set(v0, DexTypeDomain(m_type_a));
  EXPECT_EQ(env.get(v0), DexTypeDomain(m_type_a));

  reg_t v1 = 1;
  env.set(v1, DexTypeDomain(m_type_b));
  EXPECT_EQ(env.get(v1), DexTypeDomain(m_type_b));

  auto a_join_b = DexTypeDomain(m_type_a);
  a_join_b.join_with(env.get(v1));
  EXPECT_EQ(a_join_b, DexTypeDomain(m_type_a));

  auto b_join_a = DexTypeDomain(m_type_b);
  b_join_a.join_with(env.get(v0));
  EXPECT_EQ(b_join_a, DexTypeDomain(m_type_a));
}

TEST_F(DexTypeEnvironmentTest, FieldEnvTest) {
  auto env = DexTypeEnvironment();
  DexField* f1 = (DexField*)1;
  auto type = env.get(f1);
  EXPECT_TRUE(type.is_top());

  env.set(f1, DexTypeDomain(m_type_b));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_b));

  DexField* f2 = (DexField*)2;
  EXPECT_TRUE(env.get(f2).is_top());
  env.set(f2, DexTypeDomain(m_type_a));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));

  auto a_join_b = env.get(f2);
  a_join_b.join_with(env.get(f1));
  EXPECT_EQ(a_join_b, DexTypeDomain(m_type_a));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_b));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));

  auto b_join_a = env.get(f1);
  b_join_a.join_with(env.get(f2));
  EXPECT_EQ(b_join_a, DexTypeDomain(m_type_a));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_b));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));
}
