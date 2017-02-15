/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <memory>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "Creators.h"
#include "DexUtil.h"
#include "VirtualScope.h"
#include "ScopeHelper.h"

namespace {

const int OBJ_METH_NAMES = 9;

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
  auto obj_t = get_object_type();

  // class A
  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  scope.push_back(a_cls);
  // class B
  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, obj_t, {});
  scope.push_back(b_cls);

  // make sigs
  auto void_t = get_void_type();
  auto bool_t = get_boolean_type();
  auto void_void = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({}));

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
  auto void_t = get_void_type();
  auto void_void = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({}));

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
  auto void_t = get_void_type();
  auto int_t = get_int_type();
  auto bool_t = get_boolean_type();
  auto obj_t = get_object_type();
  auto void_void = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({int_t}));
  auto bool_object = DexProto::make_proto(
      bool_t, DexTypeList::make_type_list({obj_t}));

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
  auto intf1_cls = create_internal_class(
      intf1_t, get_object_type(), {}, ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf1_cls);

  // make signatures
  auto void_t = get_void_type();
  auto void_void = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({}));
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
  auto intf2_cls = create_internal_class(
      intf2_t, get_object_type(), {}, ACC_INTERFACE | ACC_PUBLIC);
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
  auto void_t = get_void_type();
  auto int_t = get_int_type();
  auto obj_t = get_object_type();
  auto void_void = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({int_t}));
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
std::vector<DexClass*> create_scope_7() {
  std::vector<DexClass*> scope = create_scope_6();
  // F.g(int)
  auto b_t = DexType::get_type("LF;");
  auto b_cls = type_class(b_t);
  auto void_t = get_void_type();
  auto int_t = get_int_type();
  auto void_int = DexProto::make_proto(
      void_t, DexTypeList::make_type_list({int_t}));
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
std::vector<DexClass*> create_scope_9() {
  std::vector<DexClass*> scope = create_scope_8();
  // interface Intf3
  auto intf2_t = DexType::make_type("LIntf2;");
  auto intf3_t = DexType::make_type("LIntf3;");
  auto intf3_cls = create_internal_class(
      intf3_t, get_object_type(), {}, ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf3_cls);
  // class D extends C implements Intf2, Intf3
  auto d_t = DexType::get_type("LD;");
  auto d_cls = type_class(d_t);
  std::deque<DexType*> d_intfs;
  d_intfs.emplace_back(intf2_t);
  d_intfs.emplace_back(intf3_t);
  d_cls->set_interfaces(DexTypeList::make_type_list(std::move(d_intfs)));
  // Intf3.f()
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
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
  auto intf4_cls = create_internal_class(
      intf4_t, get_object_type(), {}, ACC_INTERFACE | ACC_PUBLIC);
  scope.push_back(intf4_cls);

  // Intf4.f();
  create_empty_method(intf4_cls, "f",
      DexProto::make_proto(get_void_type(), DexTypeList::make_type_list({})));

  // interface Intf1 implements Intf2 { void f(); }
  type_class(intf1_t)->set_interfaces(
      DexTypeList::make_type_list(std::move(std::deque<DexType*>{intf2_t})));
  // interface Intf3 implements Intf4 { void f()); }
  type_class(intf3_t)->set_interfaces(
      DexTypeList::make_type_list(std::move(std::deque<DexType*>{intf4_t})));

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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
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
  auto m_cls = create_internal_class(
      m_t, get_object_type(), {}, ACC_PUBLIC);
  scope.push_back(m_cls);
  auto n_t = DexType::make_type("LN;");
  auto n_cls = create_internal_class(
      n_t, m_t, {esc_intf_t}, ACC_PUBLIC);
  scope.push_back(n_cls);

  // M.f(int);
  create_empty_method(m_cls, "f",
      DexProto::make_proto(get_void_type(),
          DexTypeList::make_type_list({get_int_type()})));
  // N.g(int);
  create_empty_method(n_cls, "h",
      DexProto::make_proto(get_void_type(),
          DexTypeList::make_type_list({get_int_type()})));

  return scope;
}

//
// EXPECT utility
//
template <class SigFn = void(const DexString*, const ProtoMap&)>
void for_every_sig(const SignatureMap& sig_map, SigFn fn) {
  for (const auto& sig_it : sig_map) {
    fn(sig_it.first, sig_it.second);
  }
}

template <class VirtScopesFn =
    void(const DexString*, const DexProto*, const VirtualScopes&)>
void for_every_scope(const SignatureMap& sig_map, VirtScopesFn fn) {
  for (const auto& sig_map_it : sig_map) {
    for (const auto& proto_map_it : sig_map_it.second) {
      fn(sig_map_it.first, proto_map_it.first, proto_map_it.second);
    }
  }
}

template <class VirtMethFn = void(const VirtualMethod&)>
void for_every_method(const SignatureMap& sig_map, VirtMethFn fn) {
  for (const auto& sig_map_it : sig_map) {
    for (const auto& proto_map_it : sig_map_it.second) {
      for (const auto& virt_groups : proto_map_it.second) {
        for (const auto& virt_meth : virt_groups.methods) {
          fn(virt_meth);
        }
      }
    }
  }
}

//
// Common signature map top level checks
//
void check_protos_1(SignatureMap& sm) {
  auto wait = DexString::get_string("wait");
  for_every_sig(sm,
      [&](const DexString* name, const ProtoMap& protos) {
        if (name == wait) {
          EXPECT_EQ(protos.size(), 3);
        } else {
          EXPECT_EQ(protos.size(), 1);
        }
      });
}

void check_protos_2(SignatureMap& sm) {
  auto wait = DexString::get_string("wait");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  for (const auto& sig_map_it : sm) {
    if (sig_map_it.first == wait) {
      EXPECT_EQ(sig_map_it.second.size(), 3);
    } else if (sig_map_it.first == f || sig_map_it.first == g) {
      EXPECT_EQ(sig_map_it.second.size(), 2);
    } else {
      EXPECT_EQ(sig_map_it.second.size(), 1);
    }
  }
}

//
// Helpers to check virtual scope correctness
//
// each scope is define as a
// - VirtualScope.type
// - VirtualScope.methods[0].get_class()
// - size of scope
// - type of interfaces implemented
using ScopeInfo = std::pair<size_t, std::vector<const DexType*>>;
using ExpectedScope = std::map<const DexType*, std::map<const DexType*, ScopeInfo>>;
using ExpectedProto = std::map<const DexProto*, ExpectedScope>;
using ExpectedSig = std::map<const DexString*, ExpectedProto>;

void check_expected_scopes(
    SignatureMap& sm, ExpectedSig& expected_sig) {
  for_every_scope(sm,
      [&](const DexString* name,
          const DexProto* proto,
          const VirtualScopes& scopes) {
        const auto& sig_it = expected_sig.find(name);
        if (sig_it != expected_sig.end()) {
          const auto& proto_it = sig_it->second.find(proto);
          if (proto_it != sig_it->second.end()) {
            for (const auto& scope : scopes) {
              const auto& scope_it = proto_it->second.find(scope.type);
              if (scope_it != proto_it->second.end()) {
                const auto& scope_top_it =
                    scope_it->second.find(scope.methods[0].first->get_class());
                if (scope_top_it != scope_it->second.end()) {
                  const auto size = scope_top_it->second.first;
                  const auto& intfs = scope_top_it->second.second;
                  EXPECT_EQ(scope.methods.size(), size);
                  EXPECT_EQ(intfs.size(), scope.interfaces.size());
                  for (const auto& intf : intfs) {
                    EXPECT_TRUE(scope.interfaces.count(intf));
                  }
                  continue;
                }
                // this is just to have a meaningful error message and
                // I am sure there are better ways but there you go...
                std::string msg;
                msg = msg + name->c_str() + "->" + SHOW(proto) + "->" +
                    SHOW(scope.type) + "->" + SHOW(scope.methods[0].first);
                EXPECT_STREQ("missing type scope", msg.c_str());
                continue;
              }
              // this is just to have a meaningful error message and
              // I am sure there are better ways but there you go...
              std::string msg;
              msg = msg + name->c_str() + "->" + SHOW(proto) + "->" +
                  SHOW(scope.type);
              EXPECT_STREQ("missing scope", msg.c_str());
            }
            return;
          }
          // this is just to have a meaningful error message and
          // I am sure there are better ways but there you go...
          std::string msg;
          msg = msg + name->c_str() + "->" + SHOW(proto);
          EXPECT_STREQ("missing sig", msg.c_str());
          return;
        }
        EXPECT_EQ(scopes[0].methods.size(), 1);
      });
}

//
// Helpers to check method correctness
//
using ExpectedMethod = std::map<const DexMethod*, VirtualFlags>;

// Check every method
void check_expected_methods(
    SignatureMap& sm,
    ExpectedMethod& expected_meths,
    VirtualFlags default_flags = TOP_DEF | FINAL) {
  for_every_method(sm,
      [&](const VirtualMethod& vmeth) {
        const auto& meth = vmeth.first;
        VirtualFlags flags = vmeth.second;
        const auto& expected_it = expected_meths.find(meth);
        if (expected_it != expected_meths.end()) {
          ASSERT_EQ(flags, expected_it->second);
        } else {
          ASSERT_EQ(flags, default_flags);
        }
      });
}

// Check only methods in expected map
void check_expected_methods_only(
    SignatureMap& sm, ExpectedMethod& expected_meths) {
  for_every_method(sm,
      [&](const VirtualMethod& vmeth) {
        const auto& meth = vmeth.first;
        VirtualFlags flags = vmeth.second;
        const auto& expected_it = expected_meths.find(meth);
        if (expected_it != expected_meths.end()) {
          ASSERT_EQ(flags, expected_it->second);
        }
      });
}

}

//
// Tests
//

/**
 * Simple class hierarchy
 *
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 * class B { void g() {} }
 */
TEST(NoOverload, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_1();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_1(sm);

  // check expected scopes
  for_every_scope(sm,
      [&](const DexString* name,
          const DexProto* proto,
          const VirtualScopes& scopes) {
        EXPECT_EQ(scopes.size(), 1);
        EXPECT_EQ(scopes[0].methods.size(), 1);
      });

  // check expected methods
  for_every_method(sm,
      [&](const VirtualMethod& meth) {
        ASSERT_EQ(meth.second, TOP_DEF | FINAL);
        if (meth.first->get_class() == get_object_type()) {
          ASSERT_TRUE(meth.first->is_external());
        }
      });
  delete g_redex;
}

/**
 * Simple class hierarchy with override
 *
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 * class B { void g() {} void f() {} }
 *   class C extends B { }
 *     class D extends C { void f() {} }
 *     class E extends C { void g() {} }
 */
TEST(Override, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_2();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_1(sm);

  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto e_t = DexType::get_type("LE;");
  auto d_t = DexType::get_type("LD;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  check_expected_methods(sm, expected_methods);
  delete g_redex;
}

/**
 * Simple class hierarchy with override and overload
 *
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 * class B { void g() {} void f() {} }
 *   class C extends B { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
TEST(OverrideOverload, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_3();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | FINAL;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Add interface to previous
 *
 * interface Intf1 { void f(); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
TEST(Interface, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_4();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL |FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | FINAL;
  check_expected_methods_only(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces. Add the G hierarchy
 *
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} }
 *     class G extends F { void g(int) {} }
 *       class H extends G implements Intf2 { void g(int) {} }
 *         class I extends H { void g(int) {} }
 *         class J extends H {}
 *       class K extends G { void g(int) {} }
 *     class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {} }
 */
TEST(Interface1, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_5();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {intf2_t});
  expected_sig[g][void_int][l_t][l_t] = ScopeInfo(1, {});
  expected_sig[g][void_int][g_t][g_t] = ScopeInfo(4, {intf2_t});
  expected_sig[g][void_int][intf2_t][c_t] = ScopeInfo(3, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL |FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces. Interface implemented twice on a branch
 *
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
TEST(Interface2, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_6();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");

  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {intf2_t});
  expected_sig[g][void_int][l_t][l_t] = ScopeInfo(1, {});
  expected_sig[g][void_int][g_t][g_t] = ScopeInfo(4, {intf2_t});
  expected_sig[g][void_int][intf2_t][c_t] = ScopeInfo(3, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL |FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces. Interface implemented twice on a branch and
 * with a parent not implemeting the interface
 *
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *     class G extends F { void g(int) {} }
 *       class H extends G implements Intf2 { void g(int) {} }
 *         class I extends H { void g(int) {} }
 *         class J extends H {}
 *       class K extends G { void g(int) {} }
 *     class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
TEST(Interface3, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_7();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {intf2_t});
  expected_sig[g][void_int][f_t][f_t] = ScopeInfo(6, {intf2_t});
  expected_sig[g][void_int][intf2_t][c_t] = ScopeInfo(3, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(f_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = OVERRIDE;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, g, void_int)] = TOP_DEF | IMPL;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL |FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces. Interface implemented twice on a branch and
 * one implementation missing (needs pure miranda)
 *
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 */
TEST(Interface3Miranda, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_8();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {intf2_t});
  expected_sig[g][void_int][f_t][f_t] = ScopeInfo(6, {intf2_t});
  expected_sig[g][void_int][intf2_t][c_t] = ScopeInfo(3, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(f_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = OVERRIDE;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, g, void_int)] = TOP_DEF | IMPL;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL |FINAL;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces with the same sig.
 *
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {} }
 */
TEST(Interface3MirandaMultiIntf, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_9();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto intf3_t = DexType::get_type("LIntf3;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t, intf3_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_void][intf3_t][d_t] = ScopeInfo(1, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][c_t][c_t] = ScopeInfo(3, {intf2_t});
  expected_sig[g][void_int][f_t][f_t] = ScopeInfo(6, {intf2_t});
  expected_sig[g][void_int][intf2_t][c_t] = ScopeInfo(3, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(f_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = OVERRIDE;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, g, void_int)] = TOP_DEF | IMPL;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
}

/**
 * Multiple interfaces with the same sig.
 *
 * interface Intf1 { void f(); }
 * interface Intf2 { void g(int); }
 * interface Intf3 { void f(); }
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 *   class F extends A { void f(int) {} boolean equals(Object) {} void g(int); }
 *   class G extends F { void g(int) {} }
 *     class H extends G implements Intf2 { }
 *       class I extends H { void g(int) {} }
 *       class J extends H {}
 *     class K extends G { void g(int) {} }
 *   class L extends F { void g(int) {} }
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {} }
 */
TEST(Interface3IntfOverride, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_10();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = get_object_type();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto g_t = DexType::get_type("LG;");
  auto h_t = DexType::get_type("LH;");
  auto k_t = DexType::get_type("LK;");
  auto i_t = DexType::get_type("LI;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto intf3_t = DexType::get_type("LIntf3;");
  auto intf4_t = DexType::get_type("LIntf4;");
  auto void_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto bool_obj = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t, intf3_t, intf4_t});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_void][intf3_t][d_t] = ScopeInfo(1, {});
  expected_sig[f][void_void][intf4_t][d_t] = ScopeInfo(1, {});
  expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[g][void_int][b_t][b_t] = ScopeInfo(4, {intf2_t});
  expected_sig[g][void_int][f_t][f_t] = ScopeInfo(6, {intf2_t});
  expected_sig[g][void_int][intf2_t][b_t] = ScopeInfo(4, {});
  expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
  expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF;
  expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
  expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(f_t, g, void_int)] = TOP_DEF;
  expected_methods[DexMethod::get_method(g_t, g, void_int)] = OVERRIDE;
  expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(l_t, g, void_int)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
  expected_methods[DexMethod::get_method(b_t, g, void_int)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[DexMethod::get_method(c_t, g, void_int)] = OVERRIDE | MIRANDA | IMPL;
  expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
  expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
  expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);

  delete g_redex;
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
 * class B implements Intf1 { void g() {} void f() {} void g(int) {} }
 *   class C extends B implements Intf2 { void g(int) {} }
 *     class D extends C implements Intf2, Intf3 { void f() {} void g(int) {} }
 *     class E extends C { void g() {} void g(int) {}}
 * class M { void f(int) {} }
 *   class N externds M implements EscIntf { void h(int) {}}
 */
TEST(Interface3IntfOverEscape, empty) {
   g_redex = new RedexContext();
   std::vector<DexClass*> scope = create_scope_11();
   ClassHierarchy ch = build_type_hierarchy(scope);
   SignatureMap sm = build_signature_map(ch);

   // check expected name and proto
   ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 3);
   check_protos_2(sm);

   auto eq = DexString::get_string("equals");
   auto f = DexString::get_string("f");
   auto g = DexString::get_string("g");
   auto h = DexString::get_string("h");
   auto obj_t = get_object_type();
   auto a_t = DexType::get_type("LA;");
   auto b_t = DexType::get_type("LB;");
   auto c_t = DexType::get_type("LC;");
   auto d_t = DexType::get_type("LD;");
   auto e_t = DexType::get_type("LE;");
   auto f_t = DexType::get_type("LF;");
   auto g_t = DexType::get_type("LG;");
   auto h_t = DexType::get_type("LH;");
   auto k_t = DexType::get_type("LK;");
   auto i_t = DexType::get_type("LI;");
   auto l_t = DexType::get_type("LL;");
   auto m_t = DexType::get_type("LM;");
   auto n_t = DexType::get_type("LN;");
   auto intf1_t = DexType::get_type("LIntf1;");
   auto intf2_t = DexType::get_type("LIntf2;");
   auto intf3_t = DexType::get_type("LIntf3;");
   auto intf4_t = DexType::get_type("LIntf4;");
   auto void_void = DexProto::make_proto(
       get_void_type(), DexTypeList::make_type_list({}));
   auto void_int = DexProto::make_proto(
       get_void_type(), DexTypeList::make_type_list({get_int_type()}));
   auto bool_obj = DexProto::make_proto(
       get_boolean_type(), DexTypeList::make_type_list({obj_t}));

   // check expected scopes
   ExpectedSig expected_sig;
   expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {intf1_t, intf3_t, intf4_t});
   expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
   expected_sig[f][void_void][intf1_t][b_t] = ScopeInfo(2, {});
   expected_sig[f][void_void][intf3_t][d_t] = ScopeInfo(1, {});
   expected_sig[f][void_void][intf4_t][d_t] = ScopeInfo(1, {});
   expected_sig[f][void_int][f_t][f_t] = ScopeInfo(1, {});
   expected_sig[f][void_int][m_t][m_t] = ScopeInfo(1, {});
   expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
   expected_sig[g][void_int][n_t][n_t] = ScopeInfo(1, {});
   expected_sig[g][void_int][b_t][b_t] = ScopeInfo(4, {intf2_t});
   expected_sig[g][void_int][f_t][f_t] = ScopeInfo(6, {intf2_t});
   expected_sig[g][void_int][intf2_t][b_t] = ScopeInfo(4, {});
   expected_sig[g][void_int][intf2_t][h_t] = ScopeInfo(2, {});
   expected_sig[eq][bool_obj][obj_t][obj_t] = ScopeInfo(2, {});
   check_expected_scopes(sm, expected_sig);

   // check expected methods
   ExpectedMethod expected_methods;
   expected_methods[DexMethod::get_method(obj_t, eq, bool_obj)] = TOP_DEF | ESCAPED;
   expected_methods[DexMethod::get_method(f_t, eq, bool_obj)] = OVERRIDE | FINAL | ESCAPED;
   expected_methods[DexMethod::get_method(a_t, f, void_void)] = TOP_DEF | FINAL;
   expected_methods[DexMethod::get_method(b_t, f, void_void)] = TOP_DEF | IMPL | MIRANDA;
   expected_methods[DexMethod::get_method(d_t, f, void_void)] = OVERRIDE | IMPL | FINAL | MIRANDA;
   expected_methods[DexMethod::get_method(f_t, f, void_int)] = TOP_DEF | FINAL;
   expected_methods[DexMethod::get_method(m_t, f, void_int)] = TOP_DEF | FINAL | ESCAPED;
   expected_methods[DexMethod::get_method(b_t, g, void_void)] = TOP_DEF;
   expected_methods[DexMethod::get_method(e_t, g, void_void)] = OVERRIDE | FINAL;
   expected_methods[DexMethod::get_method(f_t, g, void_int)] = TOP_DEF;
   expected_methods[DexMethod::get_method(g_t, g, void_int)] = OVERRIDE;
   expected_methods[DexMethod::get_method(i_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
   expected_methods[DexMethod::get_method(k_t, g, void_int)] = OVERRIDE | FINAL;
   expected_methods[DexMethod::get_method(l_t, g, void_int)] = OVERRIDE | FINAL;
   expected_methods[DexMethod::get_method(b_t, g, void_int)] = TOP_DEF | IMPL | MIRANDA;
   expected_methods[DexMethod::get_method(c_t, g, void_int)] = OVERRIDE | MIRANDA | IMPL;
   expected_methods[DexMethod::get_method(d_t, g, void_int)] = OVERRIDE | IMPL | FINAL | MIRANDA;
   expected_methods[DexMethod::get_method(e_t, g, void_int)] = OVERRIDE | IMPL | FINAL;
   expected_methods[DexMethod::get_method(h_t, g, void_int)] = OVERRIDE | IMPL | MIRANDA;
   expected_methods[DexMethod::get_method(m_t, f, void_int)] = TOP_DEF | FINAL | ESCAPED;
   expected_methods[DexMethod::get_method(n_t, h, void_int)] = TOP_DEF | FINAL | ESCAPED;
   check_expected_methods(sm, expected_methods, TOP_DEF | FINAL | ESCAPED);
   delete g_redex;
 }
