/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

#include "DexClass.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "TypeSystem.h"

/**
 * class java.lang.Object { // Object methods ... }
 * interface I1 {}
 * interface I1_1 implements I1 {}
 * interface I2 {}
 * interface I1_2 implements I2 {}
 * interface I3 {}
 * interface I4 {}
 * interface I1_43 implements I3, I4 {}
 * interface I1_1_43 implements I1_43 {}
 * class A { }
 *  class C extends A {}
 *    class D extends C implements I1 {}
 *    class E extends C implements I2 {}
 *    class F extends C implements I1, I2 {}
 *  class G extends A {}
 *    class H extends G {}
 *      class I extends H implements I1_1 {}
 *         class J extends I {}
 *      class L extends H implements I1_2 {}
 * class B { }
 *  class M extends B implements I4 {}
 *    class N extends M implements I1_1_43 {}
 *    class O extends M implements I1_43 {}
 *  class P extends B implements I3 {}
 *    class Q extends P {}
 *      class R extends Q implements I1_1 {}
 *      class S extends Q implements I1_2 {}
 * // external unknown type
 *  class Odd1 extends Odd implemnts IOut1 {}
 *    class Odd11 extends Odd1 implements I1 {}
 *    class Odd12 extends Odd1 {}
 *  class Odd2 extends Odd implements IOut2 {}
 */

class TypeSystemTest : public RedexTest {};

TEST_F(TypeSystemTest, empty) {
  auto const intf_flag = ACC_PUBLIC | ACC_INTERFACE;
  Scope scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();

  // interfaces
  auto i1_t = DexType::make_type("LI1;");
  auto i1_cls = create_internal_class(i1_t, obj_t, {}, intf_flag);
  scope.push_back(i1_cls);
  auto i1_1_t = DexType::make_type("LI1_1;");
  auto i1_1_cls = create_internal_class(i1_1_t, obj_t, {i1_t}, intf_flag);
  scope.push_back(i1_1_cls);
  auto i2_t = DexType::make_type("LI2;");
  auto i2_cls = create_internal_class(i2_t, obj_t, {}, intf_flag);
  scope.push_back(i2_cls);
  auto i1_2_t = DexType::make_type("LI1_2;");
  auto i1_2_cls = create_internal_class(i1_2_t, obj_t, {i2_t}, intf_flag);
  scope.push_back(i1_2_cls);
  auto i3_t = DexType::make_type("LI3;");
  auto i3_cls = create_internal_class(i3_t, obj_t, {}, intf_flag);
  scope.push_back(i3_cls);
  auto i4_t = DexType::make_type("LI4;");
  auto i4_cls = create_internal_class(i4_t, obj_t, {}, intf_flag);
  scope.push_back(i4_cls);
  auto i1_43_t = DexType::make_type("LI1_43;");
  auto i1_43_cls =
      create_internal_class(i1_43_t, obj_t, {i3_t, i4_t}, intf_flag);
  scope.push_back(i1_43_cls);
  auto i1_1_43_t = DexType::make_type("LI1_1_43;");
  auto i1_1_43_cls =
      create_internal_class(i1_1_43_t, obj_t, {i1_43_t}, intf_flag);
  scope.push_back(i1_1_43_cls);
  auto iout1_t = DexType::make_type("LIOut1;");
  auto iout2_t = DexType::make_type("LIOut2;");

  // classes
  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  scope.push_back(a_cls);
  auto c_t = DexType::make_type("LC;");
  auto c_cls = create_internal_class(c_t, a_t, {});
  scope.push_back(c_cls);
  auto d_t = DexType::make_type("LD;");
  auto d_cls = create_internal_class(d_t, c_t, {i1_t});
  scope.push_back(d_cls);
  auto e_t = DexType::make_type("LE;");
  auto e_cls = create_internal_class(e_t, c_t, {i2_t});
  scope.push_back(e_cls);
  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_internal_class(f_t, c_t, {i1_t, i2_t});
  scope.push_back(f_cls);
  auto g_t = DexType::make_type("LG;");
  auto g_cls = create_internal_class(g_t, a_t, {});
  scope.push_back(g_cls);
  auto h_t = DexType::make_type("LH;");
  auto h_cls = create_internal_class(h_t, g_t, {});
  scope.push_back(h_cls);
  auto i_t = DexType::make_type("LI;");
  auto i_cls = create_internal_class(i_t, h_t, {i1_1_t});
  scope.push_back(i_cls);
  auto j_t = DexType::make_type("LJ;");
  auto j_cls = create_internal_class(j_t, i_t, {});
  scope.push_back(j_cls);
  auto l_t = DexType::make_type("LL;");
  auto l_cls = create_internal_class(l_t, h_t, {i1_2_t});
  scope.push_back(l_cls);
  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, obj_t, {});
  scope.push_back(b_cls);
  auto m_t = DexType::make_type("LM;");
  auto m_cls = create_internal_class(m_t, b_t, {i4_t});
  scope.push_back(m_cls);
  auto n_t = DexType::make_type("LN;");
  auto n_cls = create_internal_class(n_t, m_t, {i1_1_43_t});
  scope.push_back(n_cls);
  auto o_t = DexType::make_type("LO;");
  auto o_cls = create_internal_class(o_t, m_t, {i1_43_t});
  scope.push_back(o_cls);
  auto p_t = DexType::make_type("LP;");
  auto p_cls = create_internal_class(p_t, b_t, {i3_t});
  scope.push_back(p_cls);
  auto q_t = DexType::make_type("LQ;");
  auto q_cls = create_internal_class(q_t, p_t, {});
  scope.push_back(q_cls);
  auto r_t = DexType::make_type("LR;");
  auto r_cls = create_internal_class(r_t, q_t, {i1_1_t});
  scope.push_back(r_cls);
  auto s_t = DexType::make_type("LS;");
  auto s_cls = create_internal_class(s_t, q_t, {i1_2_t});
  scope.push_back(s_cls);
  auto odd_t = DexType::make_type("LOdd;");
  auto odd1_t = DexType::make_type("LOdd1;");
  auto odd1_cls = create_internal_class(odd1_t, odd_t, {iout1_t});
  scope.push_back(odd1_cls);
  auto odd11_t = DexType::make_type("LOdd11;");
  auto odd11_cls = create_internal_class(odd11_t, odd1_t, {i1_t});
  scope.push_back(odd11_cls);
  auto odd12_t = DexType::make_type("LOdd12;");
  auto odd12_cls = create_internal_class(odd12_t, odd1_t, {});
  scope.push_back(odd12_cls);
  auto odd2_t = DexType::make_type("LOdd2;");
  auto odd2_cls = create_internal_class(odd2_t, odd_t, {iout2_t});
  scope.push_back(odd2_cls);

  TypeSystem type_system(scope);
  EXPECT_EQ(type_system.get_children(a_t).size(), 2);
  EXPECT_THAT(type_system.get_children(a_t),
              ::testing::UnorderedElementsAre(c_t, g_t));
  EXPECT_EQ(type_system.get_children(b_t).size(), 2);
  EXPECT_THAT(type_system.get_children(b_t),
              ::testing::UnorderedElementsAre(m_t, p_t));
  EXPECT_EQ(type_system.get_children(c_t).size(), 3);
  EXPECT_THAT(type_system.get_children(c_t),
              ::testing::UnorderedElementsAre(d_t, e_t, f_t));
  EXPECT_EQ(type_system.get_children(o_t).size(), 0);
  EXPECT_EQ(type_system.get_children(i_t).size(), 1);
  EXPECT_THAT(type_system.get_children(i_t),
              ::testing::UnorderedElementsAre(j_t));
  EXPECT_EQ(type_system.get_children(odd_t).size(), 2);
  EXPECT_THAT(type_system.get_children(odd_t),
              ::testing::UnorderedElementsAre(odd1_t, odd2_t));
  EXPECT_EQ(type_system.get_children(odd1_t).size(), 2);
  EXPECT_THAT(type_system.get_children(odd1_t),
              ::testing::UnorderedElementsAre(odd11_t, odd12_t));
  EXPECT_EQ(type_system.get_children(odd11_t).size(), 0);

  TypeSet types;
  type_system.get_all_children(a_t, types);
  EXPECT_EQ(types.size(), 9);
  EXPECT_THAT(types,
              ::testing::UnorderedElementsAre(
                  c_t, d_t, e_t, f_t, g_t, h_t, i_t, j_t, l_t));
  types.clear();
  type_system.get_all_children(b_t, types);
  EXPECT_EQ(types.size(), 7);
  EXPECT_THAT(
      types,
      ::testing::UnorderedElementsAre(m_t, n_t, o_t, p_t, q_t, r_t, s_t));
  types.clear();
  type_system.get_all_children(c_t, types);
  EXPECT_EQ(types.size(), 3);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(d_t, e_t, f_t));
  types.clear();
  type_system.get_all_children(o_t, types);
  EXPECT_EQ(types.size(), 0);
  types.clear();
  type_system.get_all_children(i_t, types);
  EXPECT_EQ(types.size(), 1);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(j_t));
  types.clear();
  type_system.get_all_children(odd_t, types);
  EXPECT_EQ(types.size(), 4);
  EXPECT_THAT(
      types, ::testing::UnorderedElementsAre(odd1_t, odd2_t, odd11_t, odd12_t));
  types.clear();
  type_system.get_all_children(odd1_t, types);
  EXPECT_EQ(types.size(), 2);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(odd11_t, odd12_t));
  types.clear();
  type_system.get_all_children(odd11_t, types);
  EXPECT_EQ(types.size(), 0);
  types.clear();

  EXPECT_EQ(type_system.parent_chain(a_t).size(), 2);
  EXPECT_THAT(type_system.parent_chain(a_t),
              ::testing::UnorderedElementsAre(a_t, obj_t));
  EXPECT_EQ(type_system.parent_chain(b_t).size(), 2);
  EXPECT_THAT(type_system.parent_chain(b_t),
              ::testing::UnorderedElementsAre(b_t, obj_t));
  EXPECT_EQ(type_system.parent_chain(f_t).size(), 4);
  EXPECT_THAT(type_system.parent_chain(f_t),
              ::testing::UnorderedElementsAre(f_t, c_t, a_t, obj_t));
  EXPECT_EQ(type_system.parent_chain(o_t).size(), 4);
  EXPECT_THAT(type_system.parent_chain(o_t),
              ::testing::UnorderedElementsAre(o_t, m_t, b_t, obj_t));
  EXPECT_EQ(type_system.parent_chain(j_t).size(), 6);
  EXPECT_THAT(type_system.parent_chain(j_t),
              ::testing::UnorderedElementsAre(j_t, i_t, h_t, g_t, a_t, obj_t));
  EXPECT_EQ(type_system.parent_chain(odd11_t).size(), 3);
  EXPECT_THAT(type_system.parent_chain(odd11_t),
              ::testing::UnorderedElementsAre(odd11_t, odd1_t, odd_t));
  EXPECT_EQ(type_system.parent_chain(odd2_t).size(), 2);
  EXPECT_THAT(type_system.parent_chain(odd2_t),
              ::testing::UnorderedElementsAre(odd2_t, odd_t));

  EXPECT_TRUE(type_system.is_subtype(obj_t, a_t));
  EXPECT_TRUE(type_system.is_subtype(a_t, f_t));
  EXPECT_TRUE(type_system.is_subtype(h_t, j_t));
  EXPECT_TRUE(type_system.is_subtype(m_t, o_t));
  EXPECT_TRUE(type_system.is_subtype(p_t, s_t));
  EXPECT_TRUE(type_system.is_subtype(b_t, r_t));
  EXPECT_TRUE(type_system.is_subtype(l_t, l_t));
  EXPECT_TRUE(type_system.is_subtype(odd_t, odd2_t));
  EXPECT_TRUE(type_system.is_subtype(odd1_t, odd12_t));
  EXPECT_FALSE(type_system.is_subtype(l_t, obj_t));
  EXPECT_FALSE(type_system.is_subtype(l_t, c_t));
  EXPECT_FALSE(type_system.is_subtype(o_t, m_t));
  EXPECT_FALSE(type_system.is_subtype(b_t, a_t));
  EXPECT_FALSE(type_system.is_subtype(e_t, i_t));
  EXPECT_FALSE(type_system.is_subtype(odd2_t, a_t));
  EXPECT_FALSE(type_system.is_subtype(odd12_t, odd1_t));

  EXPECT_TRUE(type_system.implements(e_t, i2_t));
  EXPECT_TRUE(type_system.implements(f_t, i2_t));
  EXPECT_TRUE(type_system.implements(f_t, i1_t));
  EXPECT_TRUE(type_system.implements(i_t, i1_t));
  EXPECT_TRUE(type_system.implements(i_t, i1_1_t));
  EXPECT_TRUE(type_system.implements(j_t, i1_t));
  EXPECT_TRUE(type_system.implements(r_t, i1_t));
  EXPECT_TRUE(type_system.implements(s_t, i2_t));
  EXPECT_TRUE(type_system.implements(n_t, i1_43_t));
  EXPECT_TRUE(type_system.implements(n_t, i4_t));
  EXPECT_TRUE(type_system.implements(n_t, i3_t));
  EXPECT_TRUE(type_system.implements(odd1_t, iout1_t));
  EXPECT_TRUE(type_system.implements(odd12_t, iout1_t));
  EXPECT_TRUE(type_system.implements(odd2_t, iout2_t));
  EXPECT_TRUE(type_system.implements(odd11_t, i1_t));
  EXPECT_FALSE(type_system.implements(e_t, i1_t));
  EXPECT_FALSE(type_system.implements(f_t, i4_t));
  EXPECT_FALSE(type_system.implements(f_t, i1_43_t));
  EXPECT_FALSE(type_system.implements(i_t, i4_t));
  EXPECT_FALSE(type_system.implements(i_t, i2_t));
  EXPECT_FALSE(type_system.implements(j_t, i3_t));
  EXPECT_FALSE(type_system.implements(r_t, i4_t));
  EXPECT_FALSE(type_system.implements(odd1_t, i1_t));
  EXPECT_FALSE(type_system.implements(odd12_t, iout2_t));
  EXPECT_FALSE(type_system.implements(odd2_t, i2_t));

  EXPECT_EQ(type_system.get_implementors(i1_t).size(), 6);
  EXPECT_THAT(
      type_system.get_implementors(i1_t),
      ::testing::UnorderedElementsAre(d_t, f_t, i_t, j_t, r_t, odd11_t));
  EXPECT_EQ(type_system.get_implementors(i2_t).size(), 4);
  EXPECT_THAT(type_system.get_implementors(i2_t),
              ::testing::UnorderedElementsAre(e_t, f_t, l_t, s_t));
  EXPECT_EQ(type_system.get_implementors(i4_t).size(), 3);
  EXPECT_THAT(type_system.get_implementors(i4_t),
              ::testing::UnorderedElementsAre(m_t, n_t, o_t));
  EXPECT_EQ(type_system.get_implementors(i1_1_43_t).size(), 1);
  EXPECT_THAT(type_system.get_implementors(i1_1_43_t),
              ::testing::UnorderedElementsAre(n_t));
  EXPECT_EQ(type_system.get_implementors(a_t).size(), 0);
  EXPECT_EQ(type_system.get_implementors(iout1_t).size(), 3);
  EXPECT_THAT(type_system.get_implementors(iout1_t),
              ::testing::UnorderedElementsAre(odd1_t, odd11_t, odd12_t));

  types.clear();
  type_system.get_all_super_interfaces(i1_2_t, types);
  EXPECT_EQ(types.size(), 1);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i2_t));
  types.clear();
  type_system.get_all_super_interfaces(i1_43_t, types);
  EXPECT_EQ(types.size(), 2);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i3_t, i4_t));
  types.clear();
  type_system.get_all_super_interfaces(i1_1_43_t, types);
  EXPECT_EQ(types.size(), 3);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i3_t, i4_t, i1_43_t));
  types.clear();
  type_system.get_all_super_interfaces(i4_t, types);
  EXPECT_EQ(types.size(), 0);
  types.clear();
  type_system.get_all_super_interfaces(iout1_t, types);
  EXPECT_EQ(types.size(), 0);

  EXPECT_EQ(type_system.get_interface_children(i1_t).size(), 1);
  EXPECT_THAT(type_system.get_interface_children(i1_t),
              ::testing::UnorderedElementsAre(i1_1_t));
  EXPECT_EQ(type_system.get_interface_children(i2_t).size(), 1);
  EXPECT_THAT(type_system.get_interface_children(i2_t),
              ::testing::UnorderedElementsAre(i1_2_t));
  EXPECT_EQ(type_system.get_interface_children(i4_t).size(), 1);
  EXPECT_THAT(type_system.get_interface_children(i4_t),
              ::testing::UnorderedElementsAre(i1_43_t));
  EXPECT_EQ(type_system.get_interface_children(i1_1_43_t).size(), 0);
  EXPECT_EQ(type_system.get_interface_children(i1_1_t).size(), 0);
  EXPECT_EQ(type_system.get_interface_children(iout1_t).size(), 0);
  EXPECT_EQ(type_system.get_interface_children(iout2_t).size(), 0);

  types.clear();
  type_system.get_all_interface_children(i1_t, types);
  EXPECT_EQ(types.size(), 1);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i1_1_t));
  types.clear();
  type_system.get_all_interface_children(i2_t, types);
  EXPECT_EQ(types.size(), 1);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i1_2_t));
  types.clear();
  type_system.get_all_interface_children(i4_t, types);
  EXPECT_EQ(types.size(), 2);
  EXPECT_THAT(types, ::testing::UnorderedElementsAre(i1_1_43_t, i1_43_t));
  types.clear();
  type_system.get_all_interface_children(i1_1_43_t, types);
  EXPECT_EQ(types.size(), 0);
  types.clear();
  type_system.get_all_interface_children(iout1_t, types);
  EXPECT_EQ(types.size(), 0);

  EXPECT_EQ(type_system.get_implemented_interfaces(f_t).size(), 2);
  EXPECT_THAT(type_system.get_implemented_interfaces(f_t),
              ::testing::UnorderedElementsAre(i1_t, i2_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(j_t).size(), 2);
  EXPECT_THAT(type_system.get_implemented_interfaces(j_t),
              ::testing::UnorderedElementsAre(i1_t, i1_1_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(n_t).size(), 4);
  EXPECT_THAT(type_system.get_implemented_interfaces(n_t),
              ::testing::UnorderedElementsAre(i1_1_43_t, i1_43_t, i3_t, i4_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(a_t).size(), 0);
  EXPECT_EQ(type_system.get_implemented_interfaces(h_t).size(), 0);
  EXPECT_EQ(type_system.get_implemented_interfaces(odd2_t).size(), 1);
  EXPECT_THAT(type_system.get_implemented_interfaces(odd2_t),
              ::testing::UnorderedElementsAre(iout2_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(odd1_t).size(), 1);
  EXPECT_THAT(type_system.get_implemented_interfaces(odd1_t),
              ::testing::UnorderedElementsAre(iout1_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(odd11_t).size(), 2);
  EXPECT_THAT(type_system.get_implemented_interfaces(odd11_t),
              ::testing::UnorderedElementsAre(iout1_t, i1_t));
  EXPECT_EQ(type_system.get_implemented_interfaces(odd12_t).size(), 1);
  EXPECT_THAT(type_system.get_implemented_interfaces(odd12_t),
              ::testing::UnorderedElementsAre(iout1_t));
  EXPECT_THAT(type_system.get_implemented_interfaces(odd_t).size(), 0);
}
