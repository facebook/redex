/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "Show.h"
#include "TypeSystem.h"
#include "VirtScopeHelper.h"
#include "VirtualScope.h"

namespace {

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
  for_every_sig(sm, [&](const DexString* name, const ProtoMap& protos) {
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
using ExpectedScope =
    std::map<const DexType*,
             std::map<const DexType*, ScopeInfo, dextypes_comparator>,
             dextypes_comparator>;
using ExpectedProto =
    std::map<const DexProto*, ExpectedScope, dexprotos_comparator>;
using ExpectedSig =
    std::map<const DexString*, ExpectedProto, dexstrings_comparator>;

void check_expected_scopes(SignatureMap& sm, ExpectedSig& expected_sig) {
  for_every_scope(
      sm,
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
using ExpectedMethod =
    std::map<const DexMethod*, VirtualFlags, dexmethods_comparator>;

// Check every method
void check_expected_methods(SignatureMap& sm,
                            ExpectedMethod& expected_meths,
                            VirtualFlags default_flags = TOP_DEF | FINAL) {
  for_every_method(sm, [&](const VirtualMethod& vmeth) {
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
void check_expected_methods_only(SignatureMap& sm,
                                 ExpectedMethod& expected_meths) {
  for_every_method(sm, [&](const VirtualMethod& vmeth) {
    const auto& meth = vmeth.first;
    VirtualFlags flags = vmeth.second;
    const auto& expected_it = expected_meths.find(meth);
    if (expected_it != expected_meths.end()) {
      ASSERT_EQ(flags, expected_it->second);
    }
  });
}

} // namespace

//
// Tests
//

class VirtScopeTest : public RedexTest {};

/**
 * Simple class hierarchy
 *
 * class java.lang.Object { // Object methods ... }
 * class A { void f() {} }
 * class B { void g() {} }
 */
TEST_F(VirtScopeTest, NoOverload) {
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
  for_every_method(sm, [&](const VirtualMethod& meth) {
    ASSERT_EQ(meth.second, TOP_DEF | FINAL);
    if (meth.first->get_class() == type::java_lang_Object()) {
      ASSERT_TRUE(meth.first->is_external());
    }
  });

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  size_t a_count = 0;
  size_t b_count = 0;
  const auto a_t = DexType::get_type("LA;");
  const auto b_t = DexType::get_type("LB;");
  cs.walk_virtual_scopes([&](const DexType* type, const VirtualScope* scope) {
    count++;
    if (type == a_t) {
      a_count++;
    } else if (type == b_t) {
      b_count++;
    }
    ASSERT_EQ(scope->methods.size(), 1);
  });
  ASSERT_EQ(count, OBJ_METHS + 2);
  ASSERT_EQ(a_count, 1);
  ASSERT_EQ(b_count, 1);
  count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet&) { count++; });
  ASSERT_EQ(count, 0);
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
TEST_F(VirtScopeTest, Override) {
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] = ScopeInfo(2, {});
  expected_sig[f][void_void][a_t][a_t] = ScopeInfo(1, {});
  expected_sig[g][void_void][b_t][b_t] = ScopeInfo(2, {});
  check_expected_scopes(sm, expected_sig);

  // check expected methods
  ExpectedMethod expected_methods;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  check_expected_methods(sm, expected_methods);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  size_t a_count = 0;
  size_t b_count = 0;
  cs.walk_virtual_scopes([&](const DexType* type, const VirtualScope* scope) {
    count++;
    if (type == a_t) {
      ASSERT_EQ(scope->methods.size(), 1);
      a_count++;
    } else if (type == b_t) {
      ASSERT_EQ(scope->methods.size(), 2);
      b_count++;
    } else {
      ASSERT_EQ(scope->methods.size(), 1);
    }
  });
  ASSERT_EQ(count, OBJ_METHS + 3);
  ASSERT_EQ(a_count, 1);
  ASSERT_EQ(b_count, 2);
  count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet&) { count++; });
  ASSERT_EQ(count, 0);
  const auto& a_scopes = cs.get(a_t);
  ASSERT_EQ(a_scopes.size(), 1);
  ASSERT_EQ(a_scopes[0]->methods[0].first,
            DexMethod::get_method(a_t, f, void_void));
  const auto& b_scopes = cs.get(b_t);
  ASSERT_EQ(b_scopes.size(), 2);
  if (b_scopes[0]->methods[0].first ==
      DexMethod::get_method(b_t, g, void_void)) {
    ASSERT_EQ(b_scopes[0]->methods[1].first,
              DexMethod::get_method(e_t, g, void_void));
    ASSERT_EQ(b_scopes[1]->methods[0].first,
              DexMethod::get_method(b_t, f, void_void));
    ASSERT_EQ(b_scopes[1]->methods[1].first,
              DexMethod::get_method(d_t, f, void_void));
  } else if (b_scopes[0]->methods[1].first ==
             DexMethod::get_method(b_t, f, void_void)) {
    ASSERT_EQ(b_scopes[0]->methods[1].first,
              DexMethod::get_method(d_t, f, void_void));
    ASSERT_EQ(b_scopes[1]->methods[0].first,
              DexMethod::get_method(b_t, g, void_void));
    ASSERT_EQ(b_scopes[1]->methods[1].first,
              DexMethod::get_method(e_t, g, void_void));
  } else {
    SUCCEED(); // cannot be
  }
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
TEST_F(VirtScopeTest, OverrideOverload) {
  std::vector<DexClass*> scope = create_scope_3();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | FINAL;
  check_expected_methods(sm, expected_methods);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  size_t a_count = 0;
  size_t f_count = 0;
  size_t b_count = 0;
  size_t c_count = 0;
  cs.walk_virtual_scopes([&](const DexType* type, const VirtualScope* scope) {
    count++;
    if (type == a_t) {
      ASSERT_EQ(scope->methods.size(), 1);
      a_count++;
    } else if (type == f_t) {
      ASSERT_EQ(scope->methods.size(), 1);
      f_count++;
    } else if (type == b_t) {
      ASSERT_EQ(scope->methods.size(), 2);
      b_count++;
    } else if (type == c_t) {
      ASSERT_EQ(scope->methods.size(), 3);
      c_count++;
    }
  });
  ASSERT_EQ(count, OBJ_METHS + 5);
  ASSERT_EQ(a_count, 1);
  ASSERT_EQ(f_count, 1);
  ASSERT_EQ(b_count, 2);
  ASSERT_EQ(c_count, 1);
  count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet&) { count++; });
  ASSERT_EQ(count, 0);
  const auto& a_scopes = cs.get(a_t);
  ASSERT_EQ(a_scopes.size(), 1);
  ASSERT_EQ(a_scopes[0]->methods[0].first,
            DexMethod::get_method(a_t, f, void_void));
  const auto& b_scopes = cs.get(b_t);
  ASSERT_EQ(b_scopes.size(), 2);
  if (b_scopes[0]->methods[0].first ==
      DexMethod::get_method(b_t, g, void_void)) {
    ASSERT_EQ(b_scopes[0]->methods[1].first,
              DexMethod::get_method(e_t, g, void_void));
    ASSERT_EQ(b_scopes[1]->methods[0].first,
              DexMethod::get_method(b_t, f, void_void));
    ASSERT_EQ(b_scopes[1]->methods[1].first,
              DexMethod::get_method(d_t, f, void_void));
  } else if (b_scopes[0]->methods[1].first ==
             DexMethod::get_method(b_t, f, void_void)) {
    ASSERT_EQ(b_scopes[0]->methods[1].first,
              DexMethod::get_method(d_t, f, void_void));
    ASSERT_EQ(b_scopes[1]->methods[0].first,
              DexMethod::get_method(b_t, g, void_void));
    ASSERT_EQ(b_scopes[1]->methods[1].first,
              DexMethod::get_method(e_t, g, void_void));
  }
  const auto& c_scopes = cs.get(c_t);
  ASSERT_EQ(c_scopes.size(), 1);
  ASSERT_EQ(c_scopes[0]->methods.size(), 3);
  const auto& d_scopes = cs.get(d_t);
  ASSERT_EQ(d_scopes.size(), 0);
  const auto& e_scopes = cs.get(e_t);
  ASSERT_EQ(e_scopes.size(), 0);
  const auto& found_scope = cs.find_virtual_scope(
      static_cast<DexMethod*>(DexMethod::get_method(e_t, g, void_int)));
  ASSERT_EQ(c_scopes[0]->type, found_scope.type);
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
TEST_F(VirtScopeTest, Interface) {
  std::vector<DexClass*> scope = create_scope_4();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
  auto a_t = DexType::get_type("LA;");
  auto b_t = DexType::get_type("LB;");
  auto c_t = DexType::get_type("LC;");
  auto d_t = DexType::get_type("LD;");
  auto e_t = DexType::get_type("LE;");
  auto f_t = DexType::get_type("LF;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | FINAL;
  check_expected_methods_only(sm, expected_methods);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet& intfs) {
    ASSERT_EQ(intfs.size(), 1);
    ASSERT_TRUE(intfs.count(intf1_t) > 0);
    count++;
  });
  ASSERT_EQ(count, 1);
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
TEST_F(VirtScopeTest, Interface1) {
  std::vector<DexClass*> scope = create_scope_5();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet&) { count++; });
  ASSERT_EQ(count, 2);
  const auto& intf1_scopes = cs.get_interface_scopes(intf1_t);
  ASSERT_EQ(intf1_scopes.size(), 1);
  ASSERT_EQ(intf1_scopes[0].size(), 1);
  ASSERT_EQ(intf1_scopes[0][0]->methods.size(), 2);
  ASSERT_EQ(intf1_scopes[0][0]->type, b_t);
  const auto& intf2_scopes = cs.get_interface_scopes(intf2_t);
  ASSERT_EQ(intf2_scopes.size(), 1);
  ASSERT_EQ(intf2_scopes[0].size(), 2);
  if (intf2_scopes[0][0]->type == c_t) {
    ASSERT_EQ(intf2_scopes[0][0]->methods.size(), 3);
    ASSERT_EQ(intf2_scopes[0][1]->methods.size(), 4);
  } else {
    ASSERT_EQ(intf2_scopes[0][0]->methods.size(), 4);
    ASSERT_EQ(intf2_scopes[0][1]->methods.size(), 3);
  }
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
TEST_F(VirtScopeTest, Interface2) {
  std::vector<DexClass*> scope = create_scope_6();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  cs.walk_all_intf_scopes([&](const DexString*,
                              const DexProto*,
                              const std::vector<const VirtualScope*>&,
                              const TypeSet&) { count++; });
  ASSERT_EQ(count, 2);
  const auto& intf1_scopes = cs.get_interface_scopes(intf1_t);
  ASSERT_EQ(intf1_scopes.size(), 1);
  ASSERT_EQ(intf1_scopes[0].size(), 1);
  ASSERT_EQ(intf1_scopes[0][0]->methods.size(), 2);
  ASSERT_EQ(intf1_scopes[0][0]->type, b_t);
  const auto& intf2_scopes = cs.get_interface_scopes(intf2_t);
  ASSERT_EQ(intf2_scopes.size(), 1);
  ASSERT_EQ(intf2_scopes[0].size(), 2);
  if (intf2_scopes[0][0]->type == c_t) {
    ASSERT_EQ(intf2_scopes[0][0]->methods.size(), 3);
    ASSERT_EQ(intf2_scopes[0][1]->methods.size(), 4);
  } else {
    ASSERT_EQ(intf2_scopes[0][0]->methods.size(), 4);
    ASSERT_EQ(intf2_scopes[0][1]->methods.size(), 3);
  }
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
TEST_F(VirtScopeTest, Interface3) {
  std::vector<DexClass*> scope = create_scope_7();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = OVERRIDE;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_int))] = TOP_DEF | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  check_expected_methods(sm, expected_methods);
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
TEST_F(VirtScopeTest, Interface3Miranda) {
  std::vector<DexClass*> scope = create_scope_8();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = OVERRIDE;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_int))] = TOP_DEF | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(d_t, f, void_void))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);
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
TEST_F(VirtScopeTest, Interface3MirandaMultiIntf) {
  std::vector<DexClass*> scope = create_scope_9();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = OVERRIDE;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_int))] = TOP_DEF | IMPL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = TOP_DEF | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, f, void_void))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);
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
TEST_F(VirtScopeTest, Interface3IntfOverride) {
  std::vector<DexClass*> scope = create_scope_10();
  ClassHierarchy ch = build_type_hierarchy(scope);
  SignatureMap sm = build_signature_map(ch);

  // check expected name and proto
  ASSERT_EQ(sm.size(), OBJ_METH_NAMES + 2);
  check_protos_2(sm);

  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] =
      ScopeInfo(2, {intf1_t, intf3_t, intf4_t});
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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = OVERRIDE;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_int))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = OVERRIDE | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, f, void_void))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  check_expected_methods(sm, expected_methods);
}

/**
 * interface Intf1 implements Intf2 { void f(); }
 * interface Intf2 { void g(int); }
 * interface Intf3 implements Intf4 { void f(); }
 * interface Intf4 { void f(); }
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
TEST_F(VirtScopeTest, Interface3IntfOverEscape) {
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
  auto obj_t = type::java_lang_Object();
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
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

  // check expected scopes
  ExpectedSig expected_sig;
  expected_sig[f][void_void][b_t][b_t] =
      ScopeInfo(2, {intf1_t, intf3_t, intf4_t});
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
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(obj_t, eq, bool_obj))] = TOP_DEF | ESCAPED;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, eq, bool_obj))] = OVERRIDE | FINAL | ESCAPED;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(a_t, f, void_void))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, f, void_void))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, f, void_void))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, f, void_int))] = TOP_DEF | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(m_t, f, void_int))] = TOP_DEF | FINAL | ESCAPED;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_void))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_void))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(f_t, g, void_int))] = TOP_DEF;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(g_t, g, void_int))] = OVERRIDE;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(i_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(k_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(l_t, g, void_int))] = OVERRIDE | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(b_t, g, void_int))] = TOP_DEF | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(c_t, g, void_int))] = OVERRIDE | MIRANDA | IMPL;
  expected_methods[static_cast<DexMethod*>(DexMethod::get_method(
      d_t, g, void_int))] = OVERRIDE | IMPL | FINAL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(e_t, g, void_int))] = OVERRIDE | IMPL | FINAL;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(h_t, g, void_int))] = OVERRIDE | IMPL | MIRANDA;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(m_t, f, void_int))] = TOP_DEF | FINAL | ESCAPED;
  expected_methods[static_cast<DexMethod*>(
      DexMethod::get_method(n_t, h, void_int))] = TOP_DEF | FINAL | ESCAPED;
  check_expected_methods(sm, expected_methods, TOP_DEF | FINAL | ESCAPED);

  // check ClassScopes
  ClassScopes cs(scope);
  size_t count = 0;
  cs.walk_all_intf_scopes([&](const DexString* name,
                              const DexProto*,
                              const std::vector<const VirtualScope*>& scopes,
                              const TypeSet& intfs) {
    if (name == f) {
      ASSERT_EQ(intfs.size(), 3);
    } else {
      ASSERT_EQ(intfs.size(), 1);
      ASSERT_EQ(scopes.size(), 2);
      if (scopes[0]->type == f_t) {
        ASSERT_EQ(scopes[1]->type, b_t);
        ASSERT_EQ(scopes[0]->methods.size(), 6);
        ASSERT_EQ(scopes[1]->methods.size(), 4);
      } else {
        ASSERT_EQ(scopes[0]->type, b_t);
        ASSERT_EQ(scopes[1]->type, f_t);
        ASSERT_EQ(scopes[0]->methods.size(), 4);
        ASSERT_EQ(scopes[1]->methods.size(), 6);
      }
    }
    count++;
  });
  ASSERT_EQ(count, 2);
  const auto& scopes = cs.get_interface_scopes(intf2_t);
  ASSERT_EQ(scopes.size(), 1);
  ASSERT_EQ(scopes[0].size(), 2);
  if (scopes[0][0]->type == f_t) {
    ASSERT_EQ(scopes[0][1]->type, b_t);
    ASSERT_EQ(scopes[0][0]->methods.size(), 6);
    ASSERT_EQ(scopes[0][1]->methods.size(), 4);
  } else {
    ASSERT_EQ(scopes[0][0]->type, b_t);
    ASSERT_EQ(scopes[0][1]->type, f_t);
    ASSERT_EQ(scopes[0][0]->methods.size(), 4);
    ASSERT_EQ(scopes[0][1]->methods.size(), 6);
  }
  const auto& g_scope = cs.find_virtual_scope(
      static_cast<DexMethod*>(DexMethod::make_method(h_t, g, void_int)));
  ASSERT_EQ(g_scope.type, f_t);
  ASSERT_EQ(g_scope.methods.size(), 6);
  const auto methods = select_from(&g_scope, g_t);
  ASSERT_EQ(methods.size(), 4);
  EXPECT_THAT(
      methods,
      ::testing::UnorderedElementsAre(
          static_cast<DexMethod*>(DexMethod::get_method(g_t, g, void_int)),
          static_cast<DexMethod*>(DexMethod::get_method(h_t, g, void_int)),
          static_cast<DexMethod*>(DexMethod::get_method(i_t, g, void_int)),
          static_cast<DexMethod*>(DexMethod::get_method(k_t, g, void_int))));
}

/**
 * Vitual/InterfaceScope resolution
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
TEST_F(VirtScopeTest, VitualInterfaceResolutionTest) {
  std::vector<DexClass*> scope = create_scope_10();
  TypeSystem type_system(scope);
  auto eq = DexString::get_string("equals");
  auto f = DexString::get_string("f");
  auto g = DexString::get_string("g");
  auto obj_t = type::java_lang_Object();
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
  auto j_t = DexType::get_type("LJ;");
  auto l_t = DexType::get_type("LL;");
  auto intf1_t = DexType::get_type("LIntf1;");
  auto intf2_t = DexType::get_type("LIntf2;");
  auto intf3_t = DexType::get_type("LIntf3;");
  auto intf4_t = DexType::get_type("LIntf4;");
  auto void_void =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto void_int = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto bool_obj = DexProto::make_proto(type::_boolean(),
                                       DexTypeList::make_type_list({obj_t}));

  // invoke_virtual I.g(int)
  // Resolve the above call and obtain G.g(int) virtual scope
  // that is where the method is introduced
  auto i_g_void_int =
      static_cast<DexMethod*>(DexMethod::get_method(i_t, g, void_int));
  const auto& g_g_virt_scope = type_system.find_virtual_scope(i_g_void_int);
  EXPECT_TRUE(g_g_virt_scope != nullptr);

  std::unordered_set<DexMethod*> methods;
  // Resolve invoke_virtual G.g(int) for I
  type_system.select_methods(*g_g_virt_scope, {i_t}, methods);
  EXPECT_EQ(*methods.begin(), i_g_void_int);
  methods.clear();
  // Resolve invoke_virtual G.g(int) for K
  type_system.select_methods(*g_g_virt_scope, {k_t}, methods);
  EXPECT_EQ(*methods.begin(), DexMethod::get_method(k_t, g, void_int));
  methods.clear();
  // Resolve invoke_virtual G.g(int) for J
  type_system.select_methods(*g_g_virt_scope, {j_t}, methods);
  EXPECT_EQ(*methods.begin(), DexMethod::get_method(g_t, g, void_int));
  methods.clear();
  // Resolve invoke_virtual G.g(int) for J, K
  type_system.select_methods(*g_g_virt_scope, {j_t, k_t}, methods);
  EXPECT_EQ(methods.size(), 2);
  EXPECT_EQ(methods.count(static_cast<DexMethod*>(
                DexMethod::get_method(g_t, g, void_int))),
            1);
  EXPECT_EQ(methods.count(static_cast<DexMethod*>(
                DexMethod::get_method(k_t, g, void_int))),
            1);
  methods.clear();

  // invoke_interface Intf2.g(int)
  // Resolve the above call and obtain Intf2.g(int) interface scope
  auto intf2_g_void_int =
      static_cast<DexMethod*>(DexMethod::get_method(intf2_t, g, void_int));
  const auto& intf2_g_intf_scope =
      type_system.find_interface_scope(intf2_g_void_int);
  EXPECT_TRUE(intf2_g_intf_scope.size() == 2);

  // Resolve invoke_interface Intf2.g(int) for I
  type_system.select_methods(intf2_g_intf_scope, {i_t}, methods);
  EXPECT_EQ(*methods.begin(), DexMethod::get_method(i_t, g, void_int));
  methods.clear();
  // Resolve invoke_interface Intf2.g(int) for E
  type_system.select_methods(intf2_g_intf_scope, {e_t}, methods);
  EXPECT_EQ(*methods.begin(), DexMethod::get_method(e_t, g, void_int));
  methods.clear();
  // Resolve invoke_interface Intf2.g(int) for E, I
  type_system.select_methods(intf2_g_intf_scope, {e_t, i_t}, methods);
  EXPECT_EQ(methods.size(), 2);
  EXPECT_EQ(methods.count(static_cast<DexMethod*>(
                DexMethod::get_method(e_t, g, void_int))),
            1);
  EXPECT_EQ(methods.count(static_cast<DexMethod*>(
                DexMethod::get_method(i_t, g, void_int))),
            1);
  methods.clear();
  // Resolve invoke_interface Intf2.g(int) for J, H
  type_system.select_methods(intf2_g_intf_scope, {j_t, h_t}, methods);
  EXPECT_EQ(methods.size(), 1);
  EXPECT_EQ(*methods.begin(), DexMethod::get_method(g_t, g, void_int));
  methods.clear();
}
