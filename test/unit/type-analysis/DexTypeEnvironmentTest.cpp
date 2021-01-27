/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexTypeEnvironment.h"

#include <boost/optional/optional_io.hpp>

#include "Creators.h"
#include "PatriciaTreeSet.h"
#include "RedexTest.h"

using TypeSet = sparta::PatriciaTreeSet<const DexType*>;

struct DexTypeEnvironmentTest : public RedexTest {
 public:
  /*
   *   Ljava/lang/Object;
   *   |
   *   A
   *  / \
   * A1  A2
   *     \
   *     A21
   *      \
   *      A211
   *
   *   Ljava/lang/Object;
   *   |
   *   B
   *   |
   *   B1
   *
   *   Ljava/lang/Object;
   *   |               \
   *   C                D
   *  /  \   \   \   \
   * C1  C2  C3  C4  C5
   *
   *
   *  Ljava/lang/Object;
   *  |
   *  Base
   *  |         \
   *  Sub1(If1) Sub2(If2)
   *  |           \
   *  Sub3(If1)   Sub4(If1, If2)
   *
   */
  DexTypeEnvironmentTest() {
    // Synthesizing Ljava/lang/Object;
    ClassCreator creator = ClassCreator(type::java_lang_Object());
    creator.create();

    m_type_a = DexType::make_type("LA");
    creator = ClassCreator(m_type_a);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_a1 = DexType::make_type("LA1");
    creator = ClassCreator(m_type_a1);
    creator.set_super(m_type_a);
    creator.create();

    m_type_a2 = DexType::make_type("LA2");
    creator = ClassCreator(m_type_a2);
    creator.set_super(m_type_a);
    creator.create();

    m_type_a21 = DexType::make_type("LA21");
    creator = ClassCreator(m_type_a21);
    creator.set_super(m_type_a2);
    creator.create();

    m_type_a211 = DexType::make_type("LA211");
    creator = ClassCreator(m_type_a211);
    creator.set_super(m_type_a21);
    creator.create();

    m_type_b = DexType::make_type("LB");
    creator = ClassCreator(m_type_b);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_b1 = DexType::make_type("LB1");
    creator = ClassCreator(m_type_b1);
    creator.set_super(m_type_b);
    creator.create();

    m_type_c = DexType::make_type("LC");
    creator = ClassCreator(m_type_c);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_c1 = DexType::make_type("LC1");
    creator = ClassCreator(m_type_c1);
    creator.set_super(m_type_c);
    creator.create();

    m_type_c2 = DexType::make_type("LC2");
    creator = ClassCreator(m_type_c2);
    creator.set_super(m_type_c);
    creator.create();

    m_type_c3 = DexType::make_type("LC3");
    creator = ClassCreator(m_type_c3);
    creator.set_super(m_type_c);
    creator.create();

    m_type_c4 = DexType::make_type("LC4");
    creator = ClassCreator(m_type_c4);
    creator.set_super(m_type_c);
    creator.create();

    m_type_c5 = DexType::make_type("LC5");
    creator = ClassCreator(m_type_c5);
    creator.set_super(m_type_c);
    creator.create();

    m_type_d = DexType::make_type("LD");
    creator = ClassCreator(m_type_d);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_base = DexType::make_type("LBase");
    creator = ClassCreator(m_type_base);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_if1 = DexType::make_type("LIf1");
    creator = ClassCreator(m_type_if1);
    creator.set_super(type::java_lang_Object());
    creator.set_access(ACC_PUBLIC | ACC_INTERFACE);
    creator.create();
    m_type_if2 = DexType::make_type("LIf2");
    creator = ClassCreator(m_type_if2);
    creator.set_super(type::java_lang_Object());
    creator.set_access(ACC_PUBLIC | ACC_INTERFACE);
    creator.create();

    m_type_sub1 = DexType::make_type("LSub1");
    creator = ClassCreator(m_type_sub1);
    creator.set_super(m_type_base);
    creator.add_interface(m_type_if1);
    creator.create();

    m_type_sub2 = DexType::make_type("LSub2");
    creator = ClassCreator(m_type_sub2);
    creator.set_super(m_type_base);
    creator.add_interface(m_type_if2);
    creator.create();

    m_type_sub3 = DexType::make_type("LSub3");
    creator = ClassCreator(m_type_sub3);
    creator.set_super(m_type_sub1);
    creator.add_interface(m_type_if1);
    creator.create();

    m_type_sub4 = DexType::make_type("LSub4");
    creator = ClassCreator(m_type_sub4);
    creator.set_super(m_type_sub2);
    creator.add_interface(m_type_if1);
    creator.add_interface(m_type_if2);
    creator.create();

    m_string_array = DexType::make_type("[Ljava/lang/String;");
    m_int_array = type::make_array_type(type::_int());
    m_sub1_array = type::make_array_type(m_type_sub1);
    m_sub2_array = type::make_array_type(m_type_sub2);
    m_sub3_array = type::make_array_type(m_type_sub3);
  }

  TypeSet get_type_set(std::initializer_list<DexType*> l) {
    TypeSet s;
    for (const auto elem : l) {
      s.insert(const_cast<const DexType*>(elem));
    }
    return s;
  }

 protected:
  DexType* m_type_a;
  DexType* m_type_a1;
  DexType* m_type_a2;
  DexType* m_type_a21;
  DexType* m_type_a211;

  DexType* m_type_b;
  DexType* m_type_b1;

  DexType* m_type_c;
  DexType* m_type_c1;
  DexType* m_type_c2;
  DexType* m_type_c3;
  DexType* m_type_c4;
  DexType* m_type_c5;
  DexType* m_type_d;

  DexType* m_type_base;
  DexType* m_type_sub1;
  DexType* m_type_sub2;
  DexType* m_type_sub3;
  DexType* m_type_sub4;
  DexType* m_type_if1;
  DexType* m_type_if2;

  DexType* m_string_array;
  DexType* m_int_array;
  DexType* m_sub1_array;
  DexType* m_sub2_array;
  DexType* m_sub3_array;
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
  env.set(v1, DexTypeDomain(m_type_a1));
  EXPECT_EQ(env.get(v1), DexTypeDomain(m_type_a1));

  auto a_join_a1 = DexTypeDomain(m_type_a);
  a_join_a1.join_with(env.get(v1));
  EXPECT_EQ(a_join_a1.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(a_join_a1.get_type_set(), get_type_set({m_type_a, m_type_a1}));

  auto a1_join_a = DexTypeDomain(m_type_a1);
  a1_join_a.join_with(env.get(v0));
  EXPECT_EQ(a1_join_a.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(a1_join_a.get_type_set(), get_type_set({m_type_a, m_type_a1}));
}

TEST_F(DexTypeEnvironmentTest, FieldEnvTest) {
  auto env = DexTypeEnvironment();
  DexField* f1 = (DexField*)1;
  auto type = env.get(f1);
  EXPECT_TRUE(type.is_top());

  env.set(f1, DexTypeDomain(m_type_a1));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_a1));

  DexField* f2 = (DexField*)2;
  EXPECT_TRUE(env.get(f2).is_top());
  env.set(f2, DexTypeDomain(m_type_a));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));

  auto a_join_a1 = env.get(f2);
  a_join_a1.join_with(env.get(f1));
  EXPECT_EQ(a_join_a1.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(a_join_a1.get_type_set(), get_type_set({m_type_a, m_type_a1}));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_a1));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));

  auto a1_join_a = env.get(f1);
  a1_join_a.join_with(env.get(f2));
  EXPECT_EQ(a1_join_a.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(a1_join_a.get_type_set(), get_type_set({m_type_a, m_type_a1}));
  EXPECT_EQ(env.get(f1), DexTypeDomain(m_type_a1));
  EXPECT_EQ(env.get(f2), DexTypeDomain(m_type_a));
}

TEST_F(DexTypeEnvironmentTest, ThisPointerEnvTest) {
  auto env = DexTypeEnvironment();
  reg_t v0 = 0;
  EXPECT_FALSE(env.is_this_ptr(v0));

  env.set_this_ptr(v0, IsDomain(true));
  EXPECT_TRUE(env.is_this_ptr(v0));

  env.set_this_ptr(v0, IsDomain(false));
  reg_t v1 = 1;
  env.set_this_ptr(v1, IsDomain(true));
  EXPECT_FALSE(env.is_this_ptr(v0));
  EXPECT_TRUE(env.is_this_ptr(v1));
}

TEST_F(DexTypeEnvironmentTest, JoinWithTest) {
  auto domain_a1 = DexTypeDomain(m_type_a1);
  auto domain_a2 = DexTypeDomain(m_type_a2);
  domain_a1.join_with(domain_a2);
  EXPECT_EQ(domain_a1.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(domain_a1.get_type_set(), get_type_set({m_type_a1, m_type_a2}));

  domain_a1 = DexTypeDomain(m_type_a1);
  auto domain_a21 = DexTypeDomain(m_type_a21);
  domain_a1.join_with(domain_a21);
  EXPECT_EQ(domain_a1.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(domain_a1.get_type_set(), get_type_set({m_type_a1, m_type_a21}));

  domain_a1 = DexTypeDomain(m_type_a1);
  auto domain_a211 = DexTypeDomain(m_type_a211);
  domain_a1.join_with(domain_a211);
  EXPECT_EQ(domain_a1.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(domain_a1.get_type_set(), get_type_set({m_type_a1, m_type_a211}));

  auto domain_a = DexTypeDomain(m_type_a);
  domain_a211 = DexTypeDomain(m_type_a211);
  domain_a.join_with(domain_a211);
  EXPECT_EQ(domain_a.get_single_domain(), SingletonDexTypeDomain(m_type_a));
  EXPECT_EQ(domain_a.get_type_set(), get_type_set({m_type_a, m_type_a211}));

  auto top1 = DexTypeDomain::top();
  auto top2 = DexTypeDomain::top();
  top1.join_with(top2);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(top2.is_top());

  domain_a = DexTypeDomain(m_type_a);
  auto domain_b = DexTypeDomain(m_type_b);
  domain_a.join_with(domain_b);
  EXPECT_EQ(domain_a.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_a.get_type_set(), get_type_set({m_type_a, m_type_b}));

  domain_a1 = DexTypeDomain(m_type_a1);
  domain_b = DexTypeDomain(m_type_b);
  domain_a1.join_with(domain_b);
  EXPECT_EQ(domain_a1.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_a1.get_type_set(), get_type_set({m_type_a1, m_type_b}));

  domain_a21 = DexTypeDomain(m_type_a21);
  domain_b = DexTypeDomain(m_type_b);
  domain_a21.join_with(domain_b);
  EXPECT_EQ(domain_a21.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_a21.get_type_set(), get_type_set({m_type_a21, m_type_b}));

  domain_a211 = DexTypeDomain(m_type_a211);
  domain_b = DexTypeDomain(m_type_b);
  domain_a211.join_with(domain_b);
  EXPECT_EQ(domain_a211.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_a211.get_type_set(), get_type_set({m_type_a211, m_type_b}));

  domain_a1 = DexTypeDomain(m_type_a1);
  auto domain_b1 = DexTypeDomain(m_type_b1);
  domain_a1.join_with(domain_b1);
  EXPECT_EQ(domain_a1.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_a1.get_type_set(), get_type_set({m_type_a1, m_type_b1}));
  EXPECT_FALSE(domain_a1.get_single_domain().is_top());
  EXPECT_FALSE(domain_b1.get_single_domain().is_top());

  domain_a1 = DexTypeDomain(m_type_a1);
  domain_b1.join_with(domain_a1);
  EXPECT_EQ(domain_b1.get_single_domain(),
            SingletonDexTypeDomain(type::java_lang_Object()));
  EXPECT_EQ(domain_b1.get_type_set(), get_type_set({m_type_a1, m_type_b1}));
  EXPECT_FALSE(domain_a1.get_single_domain().is_top());
  EXPECT_FALSE(domain_b1.get_single_domain().is_top());
}

TEST_F(DexTypeEnvironmentTest, InterfaceJoinTest) {
  auto sub1 = SingletonDexTypeDomain(m_type_sub1);
  auto sub2 = SingletonDexTypeDomain(m_type_sub2);
  sub1.join_with(sub2);
  EXPECT_TRUE(sub1.is_top());
  EXPECT_FALSE(sub2.is_top());

  sub1 = SingletonDexTypeDomain(m_type_sub1);
  sub2.join_with(sub1);
  EXPECT_TRUE(sub2.is_top());
  EXPECT_FALSE(sub1.is_top());

  sub1 = SingletonDexTypeDomain(m_type_sub1);
  auto sub3 = SingletonDexTypeDomain(m_type_sub3);
  sub1.join_with(sub3);
  EXPECT_EQ(sub1, SingletonDexTypeDomain(m_type_sub1));
  EXPECT_FALSE(sub1.is_top());
  EXPECT_FALSE(sub3.is_top());

  sub1 = SingletonDexTypeDomain(m_type_sub1);
  sub3.join_with(sub1);
  EXPECT_EQ(sub3, SingletonDexTypeDomain(m_type_sub1));
  EXPECT_FALSE(sub3.is_top());
  EXPECT_FALSE(sub1.is_top());

  sub2 = SingletonDexTypeDomain(m_type_sub2);
  auto sub4 = SingletonDexTypeDomain(m_type_sub4);
  sub2.join_with(sub4);
  EXPECT_TRUE(sub2.is_top());
  EXPECT_FALSE(sub4.is_top());

  sub2 = SingletonDexTypeDomain(m_type_sub2);
  sub4.join_with(sub2);
  EXPECT_TRUE(sub4.is_top());
  EXPECT_FALSE(sub2.is_top());

  auto base = SingletonDexTypeDomain(m_type_base);
  sub4 = SingletonDexTypeDomain(m_type_sub4);
  base.join_with(sub4);
  EXPECT_TRUE(base.is_top());
  EXPECT_FALSE(sub4.is_top());

  base = SingletonDexTypeDomain(m_type_base);
  sub4.join_with(base);
  EXPECT_TRUE(sub4.is_top());
  EXPECT_FALSE(base.is_top());
}

TEST_F(DexTypeEnvironmentTest, ExtendedInterfaceJoinTest) {
  auto sub1 = SingletonDexTypeDomain(m_type_sub1);
  auto if1 = SingletonDexTypeDomain(m_type_if1);
  sub1.join_with(if1);
  EXPECT_FALSE(sub1.is_top());
  EXPECT_EQ(sub1, SingletonDexTypeDomain(m_type_if1));
  EXPECT_FALSE(if1.is_top());

  sub1 = SingletonDexTypeDomain(m_type_sub1);
  auto if2 = SingletonDexTypeDomain(m_type_if2);
  sub1.join_with(if2);
  EXPECT_TRUE(sub1.is_top());
  EXPECT_FALSE(if2.is_top());
}

TEST_F(DexTypeEnvironmentTest, ArrayJoinTest) {
  auto sub1_array = SingletonDexTypeDomain(m_sub1_array);
  auto sub2_array = SingletonDexTypeDomain(m_sub2_array);
  sub1_array.join_with(sub2_array);
  EXPECT_TRUE(sub1_array.is_top());
  EXPECT_FALSE(sub2_array.is_top());

  sub1_array = SingletonDexTypeDomain(m_sub1_array);
  auto sub3_array = SingletonDexTypeDomain(m_sub3_array);
  sub1_array.join_with(sub3_array);
  EXPECT_FALSE(sub1_array.is_top());
  EXPECT_EQ(sub1_array, SingletonDexTypeDomain(m_sub1_array));
  EXPECT_FALSE(sub3_array.is_top());

  auto str_array = SingletonDexTypeDomain(m_string_array);
  auto int_array = SingletonDexTypeDomain(m_int_array);
  str_array.join_with(int_array);
  EXPECT_TRUE(str_array.is_top());
  EXPECT_FALSE(int_array.is_top());

  sub1_array = SingletonDexTypeDomain(m_sub1_array);
  auto sub3_nested_array =
      SingletonDexTypeDomain(type::make_array_type(m_type_sub3, 2));
  sub1_array.join_with(sub3_nested_array);
  EXPECT_TRUE(sub1_array.is_top());
  EXPECT_FALSE(sub3_nested_array.is_top());
}

TEST_F(DexTypeEnvironmentTest, NullableDexTypeDomainTest) {
  auto null1 = DexTypeDomain::null();
  EXPECT_FALSE(null1.is_bottom());
  EXPECT_FALSE(null1.is_top());
  EXPECT_TRUE(null1.get_single_domain().is_none());

  auto type_a = DexTypeDomain(m_type_a);
  null1.join_with(type_a);
  EXPECT_FALSE(null1.is_null());
  EXPECT_FALSE(null1.is_not_null());
  EXPECT_TRUE(null1.is_nullable());
  EXPECT_NE(null1, DexTypeDomain(m_type_a));
  EXPECT_EQ(*null1.get_dex_type(), m_type_a);
  EXPECT_EQ(type_a, DexTypeDomain(m_type_a));
  EXPECT_FALSE(null1.get_single_domain().is_none());
  EXPECT_FALSE(type_a.get_single_domain().is_none());

  type_a = DexTypeDomain(m_type_a);
  null1 = DexTypeDomain::null();
  type_a.join_with(null1);
  EXPECT_FALSE(type_a.is_null());
  EXPECT_FALSE(type_a.is_not_null());
  EXPECT_TRUE(type_a.is_nullable());
  EXPECT_NE(type_a, DexTypeDomain(m_type_a));
  EXPECT_EQ(*type_a.get_dex_type(), m_type_a);
  EXPECT_EQ(null1, DexTypeDomain::null());
  EXPECT_FALSE(type_a.get_single_domain().is_none());
  EXPECT_TRUE(null1.get_single_domain().is_none());

  auto top1 = DexTypeDomain::top();
  auto top2 = DexTypeDomain::top();
  top1.join_with(top2);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(top2.is_top());
  EXPECT_FALSE(top1.get_single_domain().is_none());
  EXPECT_FALSE(top2.get_single_domain().is_none());

  top1 = DexTypeDomain::top();
  auto bottom = DexTypeDomain::bottom();
  top1.join_with(bottom);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(bottom.is_bottom());
  EXPECT_FALSE(top1.get_single_domain().is_none());
  EXPECT_FALSE(bottom.get_single_domain().is_none());

  bottom = DexTypeDomain::bottom();
  top1 = DexTypeDomain::top();
  bottom.join_with(top1);
  EXPECT_TRUE(bottom.is_top());
  EXPECT_TRUE(top1.is_top());
  EXPECT_FALSE(bottom.get_single_domain().is_none());
  EXPECT_FALSE(top1.get_single_domain().is_none());
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainDeepHierarchyTest) {
  // 1 join with 1
  auto domain_a1 = SmallSetDexTypeDomain(m_type_a1);
  auto domain_a2 = SmallSetDexTypeDomain(m_type_a2);
  domain_a1.join_with(domain_a2);
  EXPECT_FALSE(domain_a1.is_top());
  EXPECT_FALSE(domain_a1.is_bottom());
  EXPECT_EQ(domain_a1.get_types(), get_type_set({m_type_a1, m_type_a2}));
  EXPECT_FALSE(domain_a2.is_top());
  EXPECT_FALSE(domain_a2.is_bottom());

  // 2 join with 1
  auto domain_a21 = SmallSetDexTypeDomain(m_type_a21);
  domain_a1.join_with(domain_a21);
  EXPECT_FALSE(domain_a1.is_top());
  EXPECT_FALSE(domain_a1.is_bottom());
  EXPECT_EQ(domain_a1.get_types(),
            get_type_set({m_type_a1, m_type_a2, m_type_a21}));
  EXPECT_FALSE(domain_a21.is_top());
  EXPECT_FALSE(domain_a21.is_bottom());

  // 3 join with 1
  auto domain_a211 = SmallSetDexTypeDomain(m_type_a211);
  domain_a1.join_with(domain_a211);
  EXPECT_FALSE(domain_a1.is_top());
  EXPECT_FALSE(domain_a1.is_bottom());
  EXPECT_EQ(domain_a1.get_types(),
            get_type_set({m_type_a1, m_type_a2, m_type_a21, m_type_a211}));
  EXPECT_FALSE(domain_a211.is_top());
  EXPECT_FALSE(domain_a211.is_bottom());

  // 4 => top
  auto domain_a = SmallSetDexTypeDomain(m_type_a);
  domain_a1.join_with(domain_a);
  EXPECT_TRUE(domain_a1.is_top());
  EXPECT_FALSE(domain_a1.is_bottom());
  EXPECT_FALSE(domain_a.is_top());
  EXPECT_FALSE(domain_a.is_bottom());

  // top and bottom
  domain_a1.set_to_top();
  EXPECT_TRUE(domain_a1.is_top());
  EXPECT_FALSE(domain_a1.is_bottom());
  EXPECT_TRUE(domain_a2.leq(domain_a1));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_a1));
  domain_a1.set_to_bottom();
  EXPECT_TRUE(domain_a1.is_bottom());
  EXPECT_FALSE(domain_a1.is_top());
  EXPECT_TRUE(domain_a1.leq(SmallSetDexTypeDomain::top()));

  // leq and equals
  EXPECT_FALSE(domain_a2.leq(domain_a21));
  EXPECT_FALSE(domain_a21.leq(domain_a2));
  EXPECT_TRUE(domain_a2.leq(SmallSetDexTypeDomain::top()));
  EXPECT_TRUE(domain_a21.leq(SmallSetDexTypeDomain::top()));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_a2));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_a21));
  EXPECT_FALSE(domain_a2.equals(domain_a21));
  EXPECT_FALSE(domain_a21.equals(domain_a2));
  EXPECT_FALSE(domain_a2.equals(SmallSetDexTypeDomain::top()));
  EXPECT_FALSE(SmallSetDexTypeDomain::top().equals(domain_a21));
  EXPECT_FALSE(domain_a2.equals(SmallSetDexTypeDomain::bottom()));
  EXPECT_FALSE(SmallSetDexTypeDomain::bottom().equals(domain_a21));
  EXPECT_FALSE(
      SmallSetDexTypeDomain::top().equals(SmallSetDexTypeDomain::bottom()));
  EXPECT_FALSE(
      SmallSetDexTypeDomain::bottom().equals(SmallSetDexTypeDomain::top()));

  auto domain_set1 = SmallSetDexTypeDomain(m_type_a1);
  domain_set1.join_with(domain_a2);
  domain_set1.join_with(domain_a21);
  domain_set1.join_with(domain_a211);
  EXPECT_TRUE(domain_a2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_a2));
  EXPECT_FALSE(domain_set1.equals(domain_a1));
  EXPECT_FALSE(domain_a1.equals(domain_set1));
  auto domain_set2 = SmallSetDexTypeDomain(m_type_a1);
  domain_set2.join_with(domain_a2);
  EXPECT_TRUE(domain_set2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_set2));
  EXPECT_FALSE(domain_set1.equals(domain_set2));
  EXPECT_FALSE(domain_set2.equals(domain_set1));

  domain_set1.join_with(domain_a);
  EXPECT_TRUE(domain_a2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_a2));
  EXPECT_TRUE(domain_set2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_set2));
  EXPECT_FALSE(domain_set1.equals(domain_set2));
  EXPECT_FALSE(domain_set2.equals(domain_set1));

  domain_set1 = SmallSetDexTypeDomain(m_type_a1);
  domain_set2 = SmallSetDexTypeDomain(m_type_a1);
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
  domain_set1.join_with(domain_a2);
  domain_set2.join_with(domain_a2);
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
  domain_set1.join_with(domain_a21);
  domain_set1.join_with(domain_a211);
  domain_set1.join_with(domain_a);
  domain_set2.join_with(domain_a21);
  domain_set2.join_with(domain_a211);
  domain_set2.join_with(domain_a);
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainFlatHierarchyTest) {
  auto domain_c1 = SmallSetDexTypeDomain(m_type_c1);
  auto domain_c2 = SmallSetDexTypeDomain(m_type_c2);
  domain_c1.join_with(domain_c2);
  EXPECT_FALSE(domain_c1.is_top());
  EXPECT_FALSE(domain_c1.is_bottom());
  EXPECT_EQ(domain_c1.get_types(), get_type_set({m_type_c1, m_type_c2}));

  auto domain_c3 = SmallSetDexTypeDomain(m_type_c3);
  domain_c1.join_with(domain_c3);
  EXPECT_FALSE(domain_c1.is_top());
  EXPECT_FALSE(domain_c1.is_bottom());
  EXPECT_EQ(domain_c1.get_types(),
            get_type_set({m_type_c1, m_type_c2, m_type_c3}));

  auto domain_c4 = SmallSetDexTypeDomain(m_type_c4);
  domain_c1.join_with(domain_c4);
  EXPECT_FALSE(domain_c1.is_top());
  EXPECT_FALSE(domain_c1.is_bottom());
  EXPECT_EQ(domain_c1.get_types(),
            get_type_set({m_type_c1, m_type_c2, m_type_c3, m_type_c4}));

  auto domain_c5 = SmallSetDexTypeDomain(m_type_c5);
  domain_c1.join_with(domain_c5);
  EXPECT_TRUE(domain_c1.is_top());
  EXPECT_FALSE(domain_c1.is_bottom());

  // set join with top => top
  auto domain_d = SmallSetDexTypeDomain(m_type_d);
  auto domain_top = domain_c1;
  EXPECT_TRUE(domain_top.is_top());
  domain_top.join_with(domain_d);
  EXPECT_TRUE(domain_top.is_top());
  EXPECT_FALSE(domain_top.is_bottom());
  EXPECT_FALSE(domain_d.is_top());
  EXPECT_FALSE(domain_d.is_bottom());
  EXPECT_EQ(domain_d.get_types(), get_type_set({m_type_d}));

  domain_top = domain_c1;
  EXPECT_TRUE(domain_top.is_top());
  domain_d.join_with(domain_top);
  EXPECT_TRUE(domain_d.is_top());
  EXPECT_FALSE(domain_d.is_bottom());
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainMixedHierarchyTest) {
  auto domain_c1 = SmallSetDexTypeDomain(m_type_c1);
  auto domain_c2 = SmallSetDexTypeDomain(m_type_c2);
  auto domain_c3 = SmallSetDexTypeDomain(m_type_c3);
  domain_c1.join_with(domain_c2);
  domain_c1.join_with(domain_c3);
  EXPECT_EQ(domain_c1.get_types(),
            get_type_set({m_type_c1, m_type_c2, m_type_c3}));

  auto domain_b = SmallSetDexTypeDomain(m_type_b);
  auto domain_b1 = SmallSetDexTypeDomain(m_type_b1);
  domain_c1.join_with(domain_b);
  EXPECT_EQ(domain_c1.get_types(),
            get_type_set({m_type_c1, m_type_c2, m_type_c3, m_type_b}));
  domain_c1.join_with(domain_b1);
  EXPECT_TRUE(domain_c1.is_top());
}

TEST_F(DexTypeEnvironmentTest, DexTypeDomainReduceProductTest) {
  auto domain = DexTypeDomain(type::java_lang_Object());
  domain.join_with(
      DexTypeDomain(type::make_array_type(type::java_lang_String())));
  EXPECT_TRUE(domain.get_single_domain().is_top());
  EXPECT_FALSE(domain.get_set_domain().is_top());
  EXPECT_EQ(domain.get_type_set(),
            get_type_set({type::java_lang_Object(),
                          type::make_array_type(type::java_lang_String())}));

  auto domain_c1 = DexTypeDomain(m_type_c1);
  domain_c1.join_with(DexTypeDomain(m_type_c2));
  domain_c1.join_with(DexTypeDomain(m_type_c3));
  domain_c1.join_with(DexTypeDomain(m_type_c4));
  domain_c1.join_with(DexTypeDomain(m_type_c5));
  EXPECT_FALSE(domain_c1.get_single_domain().is_top());
  EXPECT_TRUE(domain_c1.get_set_domain().is_top());

  domain_c1 = DexTypeDomain(m_type_c1);
  auto domain_c2 = DexTypeDomain(m_type_c2);
  domain_c2.join_with(DexTypeDomain(m_type_c3));
  domain_c2.join_with(DexTypeDomain(m_type_c4));
  domain_c2.join_with(DexTypeDomain(m_type_c5));
  EXPECT_FALSE(domain_c2.get_single_domain().is_top());
  EXPECT_FALSE(domain_c2.get_set_domain().is_top());
  domain_c1.join_with(domain_c2);
  EXPECT_FALSE(domain_c1.get_single_domain().is_top());
  EXPECT_TRUE(domain_c1.get_set_domain().is_top());
}

TEST_F(DexTypeEnvironmentTest, ConstNullnessDomainTest) {
  auto c1 = DexTypeDomain(1);
  EXPECT_FALSE(c1.is_top());
  EXPECT_EQ(*c1.get_constant(), 1);

  auto nl = DexTypeDomain::null();
  EXPECT_FALSE(nl.is_top());
  EXPECT_TRUE(nl.is_null());

  c1.join_with(nl);
  EXPECT_TRUE(c1.is_top());
  EXPECT_TRUE(c1.get<0>().get<ConstNullnessDomain>().const_domain().is_top());
  EXPECT_TRUE(c1.get_nullness().is_top());
  EXPECT_TRUE(c1.is_nullable());
}

TEST_F(DexTypeEnvironmentTest, ArrayNullnessDomainTest) {
  auto a1 = ArrayNullnessDomain(1);
  EXPECT_FALSE(a1.is_top());
  EXPECT_FALSE(a1.is_bottom());
  EXPECT_FALSE(a1.get_nullness().is_top());
  EXPECT_EQ(a1.get_nullness(), NullnessDomain(NOT_NULL));
  EXPECT_EQ(*a1.get_length(), 1);
  EXPECT_EQ(a1.get_element(0), NullnessDomain(UNINITIALIZED));
  EXPECT_TRUE(a1.get_element(1).is_top());

  auto a2 = ArrayNullnessDomain(2);
  EXPECT_FALSE(a2.is_top());
  EXPECT_FALSE(a2.is_bottom());
  EXPECT_EQ(a2.get_nullness(), NullnessDomain(NOT_NULL));
  EXPECT_EQ(*a2.get_length(), 2);
  EXPECT_EQ(a2.get_element(0), NullnessDomain(UNINITIALIZED));
  EXPECT_EQ(a2.get_element(1), NullnessDomain(UNINITIALIZED));

  a1.join_with(a2);
  EXPECT_FALSE(a1.is_top());
  EXPECT_FALSE(a1.is_bottom());
  EXPECT_EQ(a1.get_nullness(), NullnessDomain(NOT_NULL));
  EXPECT_TRUE(a1.get<1>().is_top());
  EXPECT_TRUE(a1.get_element(0).is_top());
  EXPECT_TRUE(a1.get_element(1).is_top());
}

TEST_F(DexTypeEnvironmentTest, ArrayConstNullnessDomain) {
  auto array1 = DexTypeDomain(m_string_array, 1);
  EXPECT_EQ(array1.get_array_nullness().get_nullness(),
            NullnessDomain(NOT_NULL));
  EXPECT_EQ(*array1.get_array_nullness().get_length(), 1);
  EXPECT_EQ(array1.get_array_element_nullness(0),
            NullnessDomain(UNINITIALIZED));

  array1.set_array_element_nullness(0, NullnessDomain(NOT_NULL));
  EXPECT_EQ(array1.get_array_element_nullness(0), NullnessDomain(NOT_NULL));
  EXPECT_TRUE(array1.get_array_element_nullness(1).is_top());

  array1.set_array_element_nullness(1, NullnessDomain(NOT_NULL));
  EXPECT_TRUE(array1.get_array_element_nullness(1).is_top());

  EXPECT_TRUE(array1.get_array_element_nullness(-1).is_top());
  array1.set_array_element_nullness(-1, NullnessDomain(NOT_NULL));
  EXPECT_TRUE(array1.get_array_nullness().get_elements().is_top());

  auto array2 = DexTypeDomain(m_string_array, 2);
  EXPECT_EQ(array2.get_array_nullness().get_nullness(),
            NullnessDomain(NOT_NULL));
  EXPECT_EQ(*array2.get_array_nullness().get_length(), 2);
  EXPECT_EQ(array2.get_array_element_nullness(0),
            NullnessDomain(UNINITIALIZED));
  EXPECT_EQ(array2.get_array_element_nullness(1),
            NullnessDomain(UNINITIALIZED));
  array2.set_array_element_nullness(0, NullnessDomain(NOT_NULL));
  array2.set_array_element_nullness(1, NullnessDomain(NOT_NULL));
  EXPECT_EQ(array2.get_array_element_nullness(0), NullnessDomain(NOT_NULL));
  EXPECT_EQ(array2.get_array_element_nullness(1), NullnessDomain(NOT_NULL));

  array1.join_with(array2);
  EXPECT_EQ(array2.get_array_nullness().get_nullness(),
            NullnessDomain(NOT_NULL));
  EXPECT_TRUE(array1.get_array_nullness().get<1>().is_top());
  EXPECT_TRUE(array1.get_array_nullness().get_elements().is_top());

  // DisjoinUnion crossing access
  EXPECT_FALSE(DexTypeDomain::top().get_constant());
  EXPECT_EQ(*DexTypeDomain::null().get_constant(), 0);
  EXPECT_FALSE(DexTypeDomain(m_string_array, 3).get_constant());
  EXPECT_FALSE(DexTypeDomain(m_string_array).get_constant());

  EXPECT_TRUE(DexTypeDomain::top().get_array_nullness().is_top());
  EXPECT_TRUE(DexTypeDomain::null().get_array_nullness().is_top());
  EXPECT_FALSE(DexTypeDomain(m_string_array, 3).get_array_nullness().is_top());
  EXPECT_FALSE(
      DexTypeDomain(m_string_array, 3).get_array_nullness().is_bottom());
  EXPECT_TRUE(DexTypeDomain(m_string_array).get_array_nullness().is_top());
}
