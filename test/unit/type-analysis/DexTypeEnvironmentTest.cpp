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
   * B  C
   *     \
   *     D
   *      \
   *      E
   *
   *   Ljava/lang/Object;
   *   |
   *   H
   *   |
   *   I
   *
   *   Ljava/lang/Object;
   *   |              \
   *   o              u
   *  / \  \  \  \
   * p  q  r  s  t
   *
   */
  DexTypeEnvironmentTest() {
    // Synthesizing Ljava/lang/Object;
    ClassCreator creator = ClassCreator(type::java_lang_Object());
    creator.create();

    m_type_a = DexType::make_type("A");
    creator = ClassCreator(m_type_a);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_b = DexType::make_type("B");
    creator = ClassCreator(m_type_b);
    creator.set_super(m_type_a);
    creator.create();

    m_type_c = DexType::make_type("C");
    creator = ClassCreator(m_type_c);
    creator.set_super(m_type_a);
    creator.create();

    m_type_d = DexType::make_type("D");
    creator = ClassCreator(m_type_d);
    creator.set_super(m_type_c);
    creator.create();

    m_type_e = DexType::make_type("E");
    creator = ClassCreator(m_type_e);
    creator.set_super(m_type_d);
    creator.create();

    m_type_h = DexType::make_type("H");
    creator = ClassCreator(m_type_h);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_i = DexType::make_type("I");
    creator = ClassCreator(m_type_i);
    creator.set_super(m_type_h);
    creator.create();

    m_type_o = DexType::make_type("O");
    creator = ClassCreator(m_type_o);
    creator.set_super(type::java_lang_Object());
    creator.create();

    m_type_p = DexType::make_type("P");
    creator = ClassCreator(m_type_p);
    creator.set_super(m_type_o);
    creator.create();

    m_type_q = DexType::make_type("Q");
    creator = ClassCreator(m_type_q);
    creator.set_super(m_type_o);
    creator.create();

    m_type_r = DexType::make_type("R");
    creator = ClassCreator(m_type_r);
    creator.set_super(m_type_o);
    creator.create();

    m_type_s = DexType::make_type("S");
    creator = ClassCreator(m_type_s);
    creator.set_super(m_type_o);
    creator.create();

    m_type_t = DexType::make_type("T");
    creator = ClassCreator(m_type_t);
    creator.set_super(m_type_o);
    creator.create();

    m_type_u = DexType::make_type("U");
    creator = ClassCreator(m_type_u);
    creator.set_super(type::java_lang_Object());
    creator.create();
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
  DexType* m_type_b;
  DexType* m_type_c;
  DexType* m_type_d;
  DexType* m_type_e;

  DexType* m_type_h;
  DexType* m_type_i;

  DexType* m_type_o;
  DexType* m_type_p;
  DexType* m_type_q;
  DexType* m_type_r;
  DexType* m_type_s;
  DexType* m_type_t;
  DexType* m_type_u;
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

TEST_F(DexTypeEnvironmentTest, JoinWithTest) {
  auto domain_b = DexTypeDomain(m_type_b);
  auto domain_c = DexTypeDomain(m_type_c);
  domain_b.join_with(domain_c);
  EXPECT_EQ(domain_b, DexTypeDomain(m_type_a));

  domain_b = DexTypeDomain(m_type_b);
  auto domain_d = DexTypeDomain(m_type_d);
  domain_b.join_with(domain_d);
  EXPECT_EQ(domain_b, DexTypeDomain(m_type_a));

  domain_b = DexTypeDomain(m_type_b);
  auto domain_e = DexTypeDomain(m_type_e);
  domain_b.join_with(domain_e);
  EXPECT_EQ(domain_b, DexTypeDomain(m_type_a));

  auto domain_a = DexTypeDomain(m_type_a);
  domain_e = DexTypeDomain(m_type_e);
  domain_a.join_with(domain_e);
  EXPECT_EQ(domain_a, DexTypeDomain(m_type_a));

  auto top1 = DexTypeDomain::top();
  auto top2 = DexTypeDomain::top();
  top1.join_with(top2);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(top2.is_top());

  domain_a = DexTypeDomain(m_type_a);
  auto domain_h = DexTypeDomain(m_type_h);
  domain_a.join_with(domain_h);
  EXPECT_EQ(domain_a, DexTypeDomain(type::java_lang_Object()));

  domain_b = DexTypeDomain(m_type_b);
  domain_h = DexTypeDomain(m_type_h);
  domain_b.join_with(domain_h);
  EXPECT_EQ(domain_b, DexTypeDomain(type::java_lang_Object()));

  domain_d = DexTypeDomain(m_type_d);
  domain_h = DexTypeDomain(m_type_h);
  domain_d.join_with(domain_h);
  EXPECT_EQ(domain_d, DexTypeDomain(type::java_lang_Object()));

  domain_e = DexTypeDomain(m_type_e);
  domain_h = DexTypeDomain(m_type_h);
  domain_e.join_with(domain_h);
  EXPECT_EQ(domain_e, DexTypeDomain(type::java_lang_Object()));

  domain_b = DexTypeDomain(m_type_b);
  auto domain_i = DexTypeDomain(m_type_i);
  domain_b.join_with(domain_i);
  EXPECT_TRUE(domain_b.get_type_domain().is_top());
  EXPECT_FALSE(domain_i.get_type_domain().is_top());
}

TEST_F(DexTypeEnvironmentTest, NullableDexTypeDomainTest) {
  auto null1 = DexTypeDomain::null();
  EXPECT_FALSE(null1.is_bottom());
  EXPECT_FALSE(null1.is_top());
  EXPECT_TRUE(null1.get_type_domain().is_none());

  auto type_a = DexTypeDomain(m_type_a);
  null1.join_with(type_a);
  EXPECT_FALSE(null1.is_null());
  EXPECT_FALSE(null1.is_not_null());
  EXPECT_TRUE(null1.is_nullable());
  EXPECT_NE(null1, DexTypeDomain(m_type_a));
  EXPECT_EQ(*null1.get_dex_type(), m_type_a);
  EXPECT_EQ(type_a, DexTypeDomain(m_type_a));
  EXPECT_FALSE(null1.get_type_domain().is_none());
  EXPECT_FALSE(type_a.get_type_domain().is_none());

  type_a = DexTypeDomain(m_type_a);
  null1 = DexTypeDomain::null();
  type_a.join_with(null1);
  EXPECT_FALSE(type_a.is_null());
  EXPECT_FALSE(type_a.is_not_null());
  EXPECT_TRUE(type_a.is_nullable());
  EXPECT_NE(type_a, DexTypeDomain(m_type_a));
  EXPECT_EQ(*type_a.get_dex_type(), m_type_a);
  EXPECT_EQ(null1, DexTypeDomain::null());
  EXPECT_FALSE(type_a.get_type_domain().is_none());
  EXPECT_TRUE(null1.get_type_domain().is_none());

  auto top1 = DexTypeDomain::top();
  auto top2 = DexTypeDomain::top();
  top1.join_with(top2);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(top2.is_top());
  EXPECT_FALSE(top1.get_type_domain().is_none());
  EXPECT_FALSE(top2.get_type_domain().is_none());

  top1 = DexTypeDomain::top();
  auto bottom = DexTypeDomain::bottom();
  top1.join_with(bottom);
  EXPECT_TRUE(top1.is_top());
  EXPECT_TRUE(bottom.is_bottom());
  EXPECT_FALSE(top1.get_type_domain().is_none());
  EXPECT_FALSE(bottom.get_type_domain().is_none());

  bottom = DexTypeDomain::bottom();
  top1 = DexTypeDomain::top();
  bottom.join_with(top1);
  EXPECT_TRUE(bottom.is_top());
  EXPECT_TRUE(top1.is_top());
  EXPECT_FALSE(bottom.get_type_domain().is_none());
  EXPECT_FALSE(top1.get_type_domain().is_none());
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainDeepHierarchyTest) {
  // 1 join with 1
  auto domain_b = SmallSetDexTypeDomain(m_type_b);
  auto domain_c = SmallSetDexTypeDomain(m_type_c);
  domain_b.join_with(domain_c);
  EXPECT_TRUE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_bottom());
  EXPECT_FALSE(domain_b.is_single_value());
  EXPECT_EQ(domain_b.get_types(), get_type_set({m_type_b, m_type_c}));
  EXPECT_TRUE(domain_c.is_set_value());
  EXPECT_FALSE(domain_c.is_top());
  EXPECT_FALSE(domain_c.is_bottom());
  EXPECT_FALSE(domain_c.is_single_value());

  // 2 join with 1
  auto domain_d = SmallSetDexTypeDomain(m_type_d);
  domain_b.join_with(domain_d);
  EXPECT_TRUE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_bottom());
  EXPECT_FALSE(domain_b.is_single_value());
  EXPECT_EQ(domain_b.get_types(), get_type_set({m_type_b, m_type_c, m_type_d}));
  EXPECT_TRUE(domain_d.is_set_value());
  EXPECT_FALSE(domain_d.is_top());
  EXPECT_FALSE(domain_d.is_bottom());
  EXPECT_FALSE(domain_d.is_single_value());

  // 3 join with 1
  auto domain_e = SmallSetDexTypeDomain(m_type_e);
  domain_b.join_with(domain_e);
  EXPECT_TRUE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_bottom());
  EXPECT_FALSE(domain_b.is_single_value());
  EXPECT_EQ(domain_b.get_types(),
            get_type_set({m_type_b, m_type_c, m_type_d, m_type_e}));
  EXPECT_TRUE(domain_e.is_set_value());
  EXPECT_FALSE(domain_e.is_top());
  EXPECT_FALSE(domain_e.is_bottom());
  EXPECT_FALSE(domain_e.is_single_value());

  // 4 join with 1 => single type
  auto domain_a = SmallSetDexTypeDomain(m_type_a);
  domain_b.join_with(domain_a);
  EXPECT_FALSE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_bottom());
  EXPECT_TRUE(domain_b.is_single_value());
  EXPECT_EQ(*domain_b.get_single_type(), m_type_a);
  EXPECT_TRUE(domain_b.get_types().empty());
  EXPECT_TRUE(domain_a.is_set_value());
  EXPECT_FALSE(domain_a.is_top());
  EXPECT_FALSE(domain_a.is_bottom());
  EXPECT_FALSE(domain_a.is_single_value());

  // top and bottom
  domain_b.set_to_top();
  EXPECT_TRUE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_bottom());
  EXPECT_FALSE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_single_value());
  EXPECT_TRUE(domain_c.leq(domain_b));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_b));
  domain_b.set_to_bottom();
  EXPECT_TRUE(domain_b.is_bottom());
  EXPECT_FALSE(domain_b.is_top());
  EXPECT_FALSE(domain_b.is_set_value());
  EXPECT_FALSE(domain_b.is_single_value());
  EXPECT_TRUE(domain_b.leq(SmallSetDexTypeDomain::top()));

  // leq and equals
  EXPECT_FALSE(domain_c.leq(domain_d));
  EXPECT_FALSE(domain_d.leq(domain_c));
  EXPECT_TRUE(domain_c.leq(SmallSetDexTypeDomain::top()));
  EXPECT_TRUE(domain_d.leq(SmallSetDexTypeDomain::top()));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_c));
  EXPECT_TRUE(SmallSetDexTypeDomain::bottom().leq(domain_d));
  EXPECT_FALSE(domain_c.equals(domain_d));
  EXPECT_FALSE(domain_d.equals(domain_c));
  EXPECT_FALSE(domain_c.equals(SmallSetDexTypeDomain::top()));
  EXPECT_FALSE(SmallSetDexTypeDomain::top().equals(domain_d));
  EXPECT_FALSE(domain_c.equals(SmallSetDexTypeDomain::bottom()));
  EXPECT_FALSE(SmallSetDexTypeDomain::bottom().equals(domain_d));
  EXPECT_FALSE(
      SmallSetDexTypeDomain::top().equals(SmallSetDexTypeDomain::bottom()));
  EXPECT_FALSE(
      SmallSetDexTypeDomain::bottom().equals(SmallSetDexTypeDomain::top()));

  auto domain_set1 = SmallSetDexTypeDomain(m_type_b);
  domain_set1.join_with(domain_c);
  domain_set1.join_with(domain_d);
  domain_set1.join_with(domain_e);
  EXPECT_TRUE(domain_c.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_c));
  EXPECT_FALSE(domain_set1.equals(domain_b));
  EXPECT_FALSE(domain_b.equals(domain_set1));
  auto domain_set2 = SmallSetDexTypeDomain(m_type_b);
  domain_set2.join_with(domain_c);
  EXPECT_TRUE(domain_set2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_set2));
  EXPECT_FALSE(domain_set1.equals(domain_set2));
  EXPECT_FALSE(domain_set2.equals(domain_set1));

  domain_set1.join_with(domain_a);
  EXPECT_TRUE(domain_set1.is_single_value());
  EXPECT_TRUE(domain_c.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_c));
  EXPECT_TRUE(domain_set2.leq(domain_set1));
  EXPECT_FALSE(domain_set1.leq(domain_set2));
  EXPECT_FALSE(domain_set1.equals(domain_set2));
  EXPECT_FALSE(domain_set2.equals(domain_set1));

  domain_set1 = SmallSetDexTypeDomain(m_type_b);
  domain_set2 = SmallSetDexTypeDomain(m_type_b);
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
  domain_set1.join_with(domain_c);
  domain_set2.join_with(domain_c);
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
  domain_set1.join_with(domain_d);
  domain_set1.join_with(domain_e);
  domain_set1.join_with(domain_a);
  domain_set2.join_with(domain_d);
  domain_set2.join_with(domain_e);
  domain_set2.join_with(domain_a);
  EXPECT_TRUE(domain_set1.is_single_value());
  EXPECT_TRUE(domain_set2.is_single_value());
  EXPECT_TRUE(domain_set1.equals(domain_set2));
  EXPECT_TRUE(domain_set2.equals(domain_set1));
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainFlatHierarchyTest) {
  auto domain_p = SmallSetDexTypeDomain(m_type_p);
  auto domain_q = SmallSetDexTypeDomain(m_type_q);
  domain_p.join_with(domain_q);
  EXPECT_TRUE(domain_p.is_set_value());
  EXPECT_FALSE(domain_p.is_top());
  EXPECT_FALSE(domain_p.is_bottom());
  EXPECT_FALSE(domain_p.is_single_value());
  EXPECT_EQ(domain_p.get_types(), get_type_set({m_type_p, m_type_q}));

  auto domain_r = SmallSetDexTypeDomain(m_type_r);
  domain_p.join_with(domain_r);
  EXPECT_TRUE(domain_p.is_set_value());
  EXPECT_FALSE(domain_p.is_top());
  EXPECT_FALSE(domain_p.is_bottom());
  EXPECT_FALSE(domain_p.is_single_value());
  EXPECT_EQ(domain_p.get_types(), get_type_set({m_type_p, m_type_q, m_type_r}));

  auto domain_s = SmallSetDexTypeDomain(m_type_s);
  domain_p.join_with(domain_s);
  EXPECT_TRUE(domain_p.is_set_value());
  EXPECT_FALSE(domain_p.is_top());
  EXPECT_FALSE(domain_p.is_bottom());
  EXPECT_FALSE(domain_p.is_single_value());
  EXPECT_EQ(domain_p.get_types(),
            get_type_set({m_type_p, m_type_q, m_type_r, m_type_s}));

  auto domain_t = SmallSetDexTypeDomain(m_type_t);
  domain_p.join_with(domain_t);
  EXPECT_FALSE(domain_p.is_set_value());
  EXPECT_FALSE(domain_p.is_top());
  EXPECT_FALSE(domain_p.is_bottom());
  EXPECT_TRUE(domain_p.is_single_value());
  EXPECT_EQ(*domain_p.get_single_type(), m_type_o);

  // set join with single value => single value
  auto domain_u = SmallSetDexTypeDomain(m_type_u);
  auto domain_single = domain_p;
  EXPECT_TRUE(domain_single.is_single_value());
  domain_single.join_with(domain_u);
  EXPECT_FALSE(domain_single.is_set_value());
  EXPECT_FALSE(domain_single.is_top());
  EXPECT_FALSE(domain_single.is_bottom());
  EXPECT_TRUE(domain_single.is_single_value());
  EXPECT_EQ(*domain_single.get_single_type(), type::java_lang_Object());
  EXPECT_TRUE(domain_u.is_set_value());
  EXPECT_FALSE(domain_u.is_top());
  EXPECT_FALSE(domain_u.is_bottom());
  EXPECT_FALSE(domain_u.is_single_value());
  EXPECT_EQ(domain_u.get_types(), get_type_set({m_type_u}));

  domain_single = domain_p;
  EXPECT_TRUE(domain_single.is_single_value());
  domain_u.join_with(domain_single);
  EXPECT_FALSE(domain_u.is_set_value());
  EXPECT_FALSE(domain_u.is_top());
  EXPECT_FALSE(domain_u.is_bottom());
  EXPECT_TRUE(domain_u.is_single_value());
  EXPECT_EQ(*domain_u.get_single_type(), type::java_lang_Object());
}

TEST_F(DexTypeEnvironmentTest, SmallSetDexTypeDomainMixedHierarchyTest) {
  auto domain_p = SmallSetDexTypeDomain(m_type_p);
  auto domain_q = SmallSetDexTypeDomain(m_type_q);
  auto domain_r = SmallSetDexTypeDomain(m_type_r);
  domain_p.join_with(domain_q);
  domain_p.join_with(domain_r);
  EXPECT_TRUE(domain_p.is_set_value());
  EXPECT_EQ(domain_p.get_types(), get_type_set({m_type_p, m_type_q, m_type_r}));

  auto domain_h = SmallSetDexTypeDomain(m_type_h);
  auto domain_i = SmallSetDexTypeDomain(m_type_i);
  domain_p.join_with(domain_h);
  EXPECT_TRUE(domain_p.is_set_value());
  EXPECT_EQ(domain_p.get_types(),
            get_type_set({m_type_p, m_type_q, m_type_r, m_type_h}));
  domain_p.join_with(domain_i);
  EXPECT_TRUE(domain_p.is_single_value());
  EXPECT_EQ(*domain_p.get_single_type(), type::java_lang_Object());
}
