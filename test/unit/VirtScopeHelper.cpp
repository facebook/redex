/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <memory>

#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "ScopeHelper.h"
#include "VirtualScope.h"

//
// Scopes
//

/**
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 * class B { void g() {} }
 */
std::vector<DexClass*> create_scope_1() {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();

  // class A
  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  scope.push_back(a_cls);
  // class B
  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, obj_t, {});
  scope.push_back(b_cls);

  // make sigs
  auto void_t = type::_void();
  auto bool_t = type::_boolean();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  // A.f()
  create_empty_method(a_cls, "f", void_void);
  // B.g()
  create_empty_method(b_cls, "g", void_void);

  return scope;
}

/**
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 * class B { void g() {} void f() {} }
 *   class C extends B { }
 *     class D extends C { void f() {} }
 *     class E extends C { void g() {} }
 */
std::vector<DexClass*> create_scope_2() {
  std::vector<DexClass*> scope = create_scope_1();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");

  // class C
  auto c_t = DexType::make_type("LC;");
  auto c_cls = create_internal_class(c_t, b_t, {});
  scope.push_back(c_cls);
  // class D
  auto d_t = DexType::make_type("LD;");
  auto d_cls = create_internal_class(d_t, c_t, {});
  scope.push_back(d_cls);
  // class E
  auto e_t = DexType::make_type("LE;");
  auto e_cls = create_internal_class(e_t, c_t, {});
  scope.push_back(e_cls);

  // make sigs
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  // B.f()
  auto b_cls = type_class(b_t);
  create_empty_method(b_cls, "f", void_void);
  // D.f()
  create_empty_method(d_cls, "f", void_void);
  // E.g()
  create_empty_method(e_cls, "g", void_void);

  return scope;
}

/**
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 * class B { void g() {} void f() {} }
 *   class C extends B { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_3() {
  std::vector<DexClass*> scope = create_scope_2();

  // define class F
  auto a_t = DexType::get_type("LA;");
  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_internal_class(f_t, a_t, {});
  scope.push_back(f_cls);

  // make sigs
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto bool_t = type::_boolean();
  auto obj_t = type::java_lang_Object();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));
  auto void_int =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({int_t}));
  auto bool_object =
      DexProto::make_proto(bool_t, DexTypeList::make_type_list({obj_t}));

  // C.g(int)
  auto c_t = DexType::get_type("LC;");
  auto c_cls = type_class(c_t);
  create_empty_method(c_cls, "g", void_int);
  // D.g(int)
  auto d_t = DexType::get_type("LD;");
  auto d_cls = type_class(d_t);
  create_empty_method(d_cls, "g", void_int);
  // E.g(int)
  auto e_t = DexType::get_type("LE;");
  auto e_cls = type_class(e_t);
  create_empty_method(e_cls, "g", void_int);
  // F.f(int) {}
  create_empty_method(f_cls, "f", void_int);
  // boolean F.equals(Object) {}
  create_empty_method(f_cls, "equals", bool_object);

  return scope;
}

/**
 * interface Intf1 { void f(); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_4() {
  std::vector<DexClass*> scope = create_scope_3();

  // interface Intf1
  auto intf1_t = DexType::make_type("LIntf1;");
  auto intf1_cls = create_internal_class(intf1_t, type::java_lang_Object(), {},
                                         ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf1_cls);

  // make signatures
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));
  create_abstract_method(intf1_cls, "f", void_void);

  // add interface to B
  auto b_t = DexType::get_type("LB;");
  auto b_cls = type_class(b_t);
  b_cls->set_interfaces(DexTypeList::make_type_list({intf1_t}));

  return scope;
}

/**
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { void g(int) {} }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_5() {
  std::vector<DexClass*> scope = create_scope_4();
  // interface Intf2
  auto intf2_t = DexType::make_type("LIntf2;");
  auto intf2_cls = create_internal_class(intf2_t, type::java_lang_Object(), {},
                                         ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf2_cls);
  // class C extends B implements Intf2
  auto c_t = DexType::get_type("LC;");
  auto c_cls = type_class(c_t);
  std::deque<DexType*> c_intfs;
  c_intfs.emplace_back(intf2_t);
  c_cls->set_interfaces(DexTypeList::make_type_list(std::move(c_intfs)));
  // class G extends F { void g(int) {} }
  //   class H extends G implements Intf2 { void g(int) {} }
  //      class I extends H { void g(int) {} }
  //      class J extends H {}
  //    class K extends G { void g(int) {} }
  //  class L extends F { void g(int) {} }
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::make_type("LG;");
  auto g_cls = create_internal_class(g_t, f_t, {});
  scope.push_back(g_cls);
  auto h_t = DexType::make_type("LH;");
  auto h_cls = create_internal_class(h_t, g_t, {intf2_t});
  scope.push_back(h_cls);
  auto i_t = DexType::make_type("LI;");
  auto i_cls = create_internal_class(i_t, h_t, {});
  scope.push_back(i_cls);
  auto j_t = DexType::make_type("LJ;");
  auto j_cls = create_internal_class(j_t, h_t, {});
  scope.push_back(j_cls);
  auto k_t = DexType::make_type("LK;");
  auto k_cls = create_internal_class(k_t, g_t, {});
  scope.push_back(k_cls);
  auto l_t = DexType::make_type("LL;");
  auto l_cls = create_internal_class(l_t, f_t, {});
  scope.push_back(l_cls);

  // make sigs
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto obj_t = type::java_lang_Object();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));
  auto void_int =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({int_t}));
  // Intf2.g(int)
  create_abstract_method(intf2_cls, "g", void_int);
  // G.g(int)
  create_empty_method(g_cls, "g", void_int);
  // H.g(int)
  create_empty_method(h_cls, "g", void_int);
  // I.g(int)
  create_empty_method(i_cls, "g", void_int);
  // K.g(int)
  create_empty_method(k_cls, "g", void_int);
  // L.g(int)
  create_empty_method(l_cls, "g", void_int);

  return scope;
}

/**
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { void g(int) {} }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_6() {
  std::vector<DexClass*> scope = create_scope_5();
  auto intf2_t = DexType::get_type("LIntf2;");
  // class C extends B implements Intf2
  auto d_t = DexType::get_type("LD;");
  auto d_cls = type_class(d_t);
  std::deque<DexType*> d_intfs;
  d_intfs.emplace_back(intf2_t);
  d_cls->set_interfaces(DexTypeList::make_type_list(std::move(d_intfs)));

  return scope;
}

/* clang-format off */

/**
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int) {} }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { void g(int) {} }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */

/* clang-format on */

std::vector<DexClass*> create_scope_7() {
  std::vector<DexClass*> scope = create_scope_6();
  // F.g(int)
  auto b_t = DexType::get_type("LF;");
  auto b_cls = type_class(b_t);
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto void_int =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({int_t}));
  create_empty_method(b_cls, "g", void_int);

  return scope;
}

/**
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_8() {
  std::vector<DexClass*> scope = create_scope_7();
  // remove H.g(int) - which is all H methods
  auto h_t = DexType::get_type("LH;");
  auto h_cls = type_class(h_t);
  for (auto& vmeth : h_cls->get_vmethods()) {
    g_redex->erase_method(vmeth);
  }
  h_cls->get_vmethods().clear();

  return scope;
}

/**
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * interface Intf3 { void f()); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_9() {
  std::vector<DexClass*> scope = create_scope_8();
  // interface Intf3
  auto intf2_t = DexType::make_type("LIntf2;");
  auto intf3_t = DexType::make_type("LIntf3;");
  auto intf3_cls = create_internal_class(intf3_t, type::java_lang_Object(), {},
                                         ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf3_cls);
  // class D extends C implements Intf2, Intf3
  auto d_t = DexType::get_type("LD;");
  auto d_cls = type_class(d_t);
  std::deque<DexType*> d_intfs;
  d_intfs.emplace_back(intf2_t);
  d_intfs.emplace_back(intf3_t);
  d_cls->set_interfaces(DexTypeList::make_type_list(std::move(d_intfs)));
  // Intf3.f()
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  create_abstract_method(intf3_cls, "f", void_void);
  return scope;
}

/**
 * interface Intf1 implements Intf2 { void f(); }
 * interface Intf2 { void g(int); }
 * interface Intf3 implements Intf4 { void f()); }
 * interface Intf4 { void f()); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *     class G extends F { void g(int) {} }
 *       class H extends G implements Intf2 { }
 *         class I extends H { void g(int) {} }
 *         class J extends H {}
 *       class K extends G { void g(int) {} }
 *     class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_10() {
  std::vector<DexClass*> scope = create_scope_9();

  auto intf1_t = DexType::make_type("LIntf1;");
  auto intf2_t = DexType::make_type("LIntf2;");
  auto intf3_t = DexType::make_type("LIntf3;");

  // interface Intf4
  auto intf4_t = DexType::make_type("LIntf4;");
  auto intf4_cls = create_internal_class(intf4_t, type::java_lang_Object(), {},
                                         ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf4_cls);

  // Intf4.f();
  create_empty_method(
      intf4_cls, "f",
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({})));

  // interface Intf1 implements Intf2 { void f(); }
  type_class(intf1_t)->set_interfaces(
      DexTypeList::make_type_list(std::deque<DexType*>{intf2_t}));
  // interface Intf3 implements Intf4 { void f()); }
  type_class(intf3_t)->set_interfaces(
      DexTypeList::make_type_list(std::deque<DexType*>{intf4_t}));

  return scope;
}

/**
 * interface Intf1 implements Intf2 { void f(); }
 * interface Intf2 { void g(int); }
 * interface Intf3 implements Intf4 { void f()); }
 * interface Intf4 { void f()); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *     class G extends F { void g(int) {} }
 *       class H extends G implements Intf2 { }
 *         class I extends H { void g(int) {} }
 *         class J extends H {}
 *       class K extends G { void g(int) {} }
 *     class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 * class M { void f(int) {} }
 *   class N externds M implements EscIntf { void h(int) {} }
 */
std::vector<DexClass*> create_scope_11() {
  std::vector<DexClass*> scope = create_scope_10();

  // external/escaped interface  EscIntf
  auto esc_intf_t = DexType::make_type("LEscIntf;");
  // class M
  auto m_t = DexType::make_type("LM;");
  auto m_cls =
      create_internal_class(m_t, type::java_lang_Object(), {}, ACC_PUBLIC);
  scope.push_back(m_cls);
  auto n_t = DexType::make_type("LN;");
  auto n_cls = create_internal_class(n_t, m_t, {esc_intf_t}, ACC_PUBLIC);
  scope.push_back(n_cls);

  // M.f(int);
  create_empty_method(
      m_cls, "f",
      DexProto::make_proto(type::_void(),
                           DexTypeList::make_type_list({type::_int()})));
  // N.g(int);
  create_empty_method(
      n_cls, "h",
      DexProto::make_proto(type::_void(),
                           DexTypeList::make_type_list({type::_int()})));

  return scope;
}
