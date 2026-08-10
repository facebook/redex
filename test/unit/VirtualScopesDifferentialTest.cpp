/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Differential oracle: for each internal type, the ClassMerging-relevant view
// of the legacy `virt_scope::ClassScopes::get(type)` must match the new
// `virtual_scope::VirtualScopes::at(type)`. This is the NFC contract for the
// ClassMerging migration -- Model.cpp classifies each scope as interface /
// non-virtual / virtual and collects its methods, so if the per-type scope sets
// agree (kind + method set + interface set), the migration is
// behavior-preserving. This is the NFC guard for the migration: VirtualScopes
// is MOG-backed (it does NOT delegate to legacy ClassScopes), and this oracle
// asserts it reproduces legacy `ClassScopes::get` exactly. Being MOG-backed
// rather than a facade over legacy is what lets the legacy VirtualScope /
// ClassScopes machinery be deleted once all consumers migrate.

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "ScopeHelper.h"
#include "Show.h"
#include "TypeUtil.h"
#include "VirtualScope.h" // legacy virt_scope
#include "VirtualScopes.h" // new virtual_scope

namespace {

DexProto* void_proto() {
  return DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
}

DexProto* void_int_proto() {
  return DexProto::make_proto(type::_void(),
                              DexTypeList::make_type_list({type::_int()}));
}

DexClass* make_class(Scope& scope,
                     const char* name,
                     DexType* super,
                     const std::vector<DexType*>& interfaces = {},
                     DexAccessFlags access = ACC_PUBLIC) {
  auto* cls = create_internal_class(DexType::make_type(name), super, interfaces,
                                    access);
  scope.push_back(cls);
  return cls;
}

std::string join(const std::vector<std::string>& parts) {
  // Scope order within at(type) is compared as-is (it drives ClassMerging's
  // collection order).
  std::string out;
  for (const auto& p : parts) {
    out += p;
    out += "\n";
  }
  return out;
}

std::string scope_str(char kind,
                      const std::vector<std::string>& methods,
                      std::vector<std::string> intfs,
                      bool has_def) {
  // methods are compared in order (order is behaviorally significant);
  // interface sets are order-insensitive, so sort them for comparison.
  std::sort(intfs.begin(), intfs.end());
  std::string s(1, kind);
  s += has_def ? " has_def=1" : " has_def=0";
  s += " methods=[";
  for (size_t i = 0; i < methods.size(); i++) {
    if (i != 0) {
      s += ", ";
    }
    s += methods[i];
  }
  s += "] intfs=[";
  for (size_t i = 0; i < intfs.size(); i++) {
    if (i != 0) {
      s += ", ";
    }
    s += intfs[i];
  }
  s += "]";
  return s;
}

std::string legacy_summary(const virt_scope::ClassScopes& cs,
                           const DexType* t) {
  std::vector<std::string> lines;
  for (const auto* s : cs.get(t)) {
    char kind = virt_scope::is_impl_scope(s)          ? 'I'
                : virt_scope::is_non_virtual_scope(s) ? 'N'
                                                      : 'V';
    std::vector<std::string> ms;
    ms.reserve(s->methods.size());
    for (const auto& m : s->methods) {
      ms.emplace_back(SHOW(m.first));
    }
    std::vector<std::string> is;
    is.reserve(s->interfaces.size());
    for (const auto* i : s->interfaces) {
      is.emplace_back(SHOW(i));
    }
    lines.push_back(scope_str(kind, ms, is, s->has_def()));
  }
  return join(lines);
}

std::string new_summary(const virtual_scope::VirtualScopes& vs,
                        const DexType* t) {
  std::vector<std::string> lines;
  for (const auto* s : vs.at(t)) {
    char kind = s->implements_interface()   ? 'I'
                : s->is_effectively_final() ? 'N'
                                            : 'V';
    std::vector<std::string> ms;
    ms.reserve(s->methods().size());
    for (const auto* m : s->methods()) {
      ms.emplace_back(SHOW(m));
    }
    std::vector<std::string> is;
    is.reserve(s->implemented_interfaces().size());
    for (const auto* i : UnorderedIterable(s->implemented_interfaces())) {
      is.emplace_back(SHOW(i));
    }
    lines.push_back(scope_str(kind, ms, is, s->has_def()));
  }
  return join(lines);
}

// Compare legacy vs new for every non-interface class in `scope`, INCLUDING
// external classes -- at(externalType) (e.g. java.lang.Object) must match
// legacy get(externalType), since ClassMerging's distribute walks parent_chain
// up to Object. Skipping external types here is the hole that hid a non-NFC
// gap.
void expect_parity(Scope& scope) {
  virt_scope::ClassScopes cs(scope);
  virtual_scope::VirtualScopes vs(scope);
  for (auto* cls : scope) {
    if (is_interface(cls)) {
      continue;
    }
    const auto* t = cls->get_type();
    SCOPED_TRACE(std::string("type ") + SHOW(t));
    if (getenv("VSCOPE_DUMP") != nullptr) {
      fprintf(stderr, "DUMP %s\n  legacy: %s\n  new:    %s\n", SHOW(t),
              legacy_summary(cs, t).c_str(), new_summary(vs, t).c_str());
    }
    EXPECT_EQ(legacy_summary(cs, t), new_summary(vs, t));
  }
}

DexType* obj() { return type::java_lang_Object(); }

} // namespace

class VirtualScopesDifferentialTest : public RedexTest {};

TEST_F(VirtualScopesDifferentialTest, SimpleOverride) {
  auto scope = create_empty_scope();
  auto* a = make_class(scope, "LA;", obj());
  create_empty_method(a, "m", void_proto());
  auto* b = make_class(scope, "LB;", a->get_type());
  create_empty_method(b, "m", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, OverrideChain) {
  auto scope = create_empty_scope();
  auto* a = make_class(scope, "LA;", obj());
  create_empty_method(a, "m", void_proto());
  auto* b = make_class(scope, "LB;", a->get_type());
  create_empty_method(b, "m", void_proto());
  auto* c = make_class(scope, "LC;", b->get_type());
  create_empty_method(c, "m", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, InterfaceImplementedOnClass) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  create_empty_method(c, "n", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, InterfaceImplInSubclass) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "n", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, SiblingInterfaceImpls) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "n", void_proto());
  auto* e = make_class(scope, "LE;", c->get_type());
  create_empty_method(e, "n", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, PureUnimplementedInterface) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  make_class(scope, "LC;", obj(), {i->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, AbstractRootConcreteOverride) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  create_abstract_method(c, "n", void_proto());
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "n", void_proto());
  expect_parity(scope);
}

TEST_F(VirtualScopesDifferentialTest, ExternalBaseOverride) {
  auto scope = create_empty_scope();
  auto* e = create_external_class(DexType::make_type("LExt;"), obj(), {});
  scope.push_back(e);
  create_abstract_method(e, "m", void_proto());
  auto* c = make_class(scope, "LC;", e->get_type());
  create_empty_method(c, "m", void_proto());
  expect_parity(scope);
}

// C implements I (abstract, no n); D extends C re-declares `implements I` and
// defines n. Legacy marks the re-declaring class's method miranda too.
TEST_F(VirtualScopesDifferentialTest, RedundantImplements) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* d = make_class(scope, "LD;", c->get_type(), {i->get_type()});
  create_empty_method(d, "n", void_proto());
  expect_parity(scope);
}

// interface Base{n}; interface Derived extends Base; class C implements
// Derived.
TEST_F(VirtualScopesDifferentialTest, InterfaceExtendsInterface) {
  auto scope = create_empty_scope();
  auto* base = make_class(scope, "LBase;", obj(), {},
                          ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(base, "n", void_proto());
  auto* derived = make_class(scope, "LDerived;", obj(), {base->get_type()},
                             ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  auto* c = make_class(scope, "LC;", obj(), {derived->get_type()});
  create_empty_method(c, "n", void_proto());
  expect_parity(scope);
}

// I{n}, J{n} (same sig); A implements I with n; B extends A implements J,
// overriding n. One scope satisfies both interfaces.
TEST_F(VirtualScopesDifferentialTest, MultiInterfaceSameSig) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* j = make_class(scope, "LJ;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(j, "n", void_proto());
  auto* a = make_class(scope, "LA;", obj(), {i->get_type()});
  create_empty_method(a, "n", void_proto());
  auto* b = make_class(scope, "LB;", a->get_type(), {j->get_type()});
  create_empty_method(b, "n", void_proto());
  expect_parity(scope);
}

// A{f(), f(int)}; B extends A overrides both. Overloads keyed by proto.
TEST_F(VirtualScopesDifferentialTest, Overloads) {
  auto scope = create_empty_scope();
  auto* a = make_class(scope, "LA;", obj());
  create_empty_method(a, "f", void_proto());
  create_empty_method(a, "f", void_int_proto());
  auto* b = make_class(scope, "LB;", a->get_type());
  create_empty_method(b, "f", void_proto());
  create_empty_method(b, "f", void_int_proto());
  expect_parity(scope);
}

// A{n} does NOT implement I; B extends A implements I but defines no n -- the
// obligation is satisfied by the inherited A.n.
TEST_F(VirtualScopesDifferentialTest, InheritedImplSatisfiesInterface) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* a = make_class(scope, "LA;", obj());
  create_empty_method(a, "n", void_proto());
  make_class(scope, "LB;", a->get_type(), {i->get_type()});
  expect_parity(scope);
}

// interface I{n} implemented by two independent branches: C{n} and D{n}.
TEST_F(VirtualScopesDifferentialTest,
       TwoIndependentBranchesImplementInterface) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  create_empty_method(c, "n", void_proto());
  auto* d = make_class(scope, "LD;", obj(), {i->get_type()});
  create_empty_method(d, "n", void_proto());
  expect_parity(scope);
}

// A{m}; B extends A{m} implements I{m}; C extends B (no m); D extends C{m}.
// Mixes a class-override chain with an interface obligation and a non-defining
// middle.
TEST_F(VirtualScopesDifferentialTest, DeepMixedHierarchy) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "m", void_proto());
  auto* a = make_class(scope, "LA;", obj());
  create_empty_method(a, "m", void_proto());
  auto* b = make_class(scope, "LB;", a->get_type(), {i->get_type()});
  create_empty_method(b, "m", void_proto());
  auto* c = make_class(scope, "LC;", b->get_type());
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "m", void_proto());
  expect_parity(scope);
}

// abstract A{abstract m}; B extends A{concrete m}. No interface involved.
TEST_F(VirtualScopesDifferentialTest, AbstractClassNoInterface) {
  auto scope = create_empty_scope();
  auto* a = make_class(scope, "LA;", obj(), {}, ACC_PUBLIC | ACC_ABSTRACT);
  create_abstract_method(a, "m", void_proto());
  auto* b = make_class(scope, "LB;", a->get_type());
  create_empty_method(b, "m", void_proto());
  expect_parity(scope);
}

// interface I{m}; EXTERNAL abstract class ExtAbs implements I but defines no m
// (framework-like, e.g. android.view.View); internal C extends ExtAbs defines
// m. The dispatch obligation for m is rooted at the external abstract ancestor,
// so C.m does not root a scope at C (get(C) empty) -- the model must recognize
// the miranda anchor even on an external class.
TEST_F(VirtualScopesDifferentialTest, ExternalAbstractImplementsInterface) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "m", void_proto());
  auto* e = create_external_class(DexType::make_type("LExtAbs;"), obj(),
                                  {i->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  scope.push_back(e);
  auto* c = make_class(scope, "LC;", e->get_type());
  create_empty_method(c, "m", void_proto());
  expect_parity(scope);
}

// ---- Interface `default` (concrete) method shapes (real-app divergences) ----
// A default method is a concrete vmethod on the interface. create_empty_method
// on an interface class produces exactly that.

// I{default d}; C implements I, no own d. (SafeIterableMap.forEach-like)
TEST_F(VirtualScopesDifferentialTest, DefaultMethodNoImpl) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_empty_method(i, "d", void_proto());
  make_class(scope, "LC;", obj(), {i->get_type()});
  expect_parity(scope);
}

// I{default d}; C implements I (no d); C overrides d. (ArrayMap.compute-like)
TEST_F(VirtualScopesDifferentialTest, DefaultMethodOwnOverride) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  create_empty_method(c, "d", void_proto());
  expect_parity(scope);
}

// I{default d}; C implements I (no d); D extends C overrides d.
// (EmptyActivityLifecycleCallbacks.onActivityPostResumed-like: default on a
// DIRECT interface, descendant overrides.)
TEST_F(VirtualScopesDifferentialTest, DefaultMethodDescendantOverridesDirect) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "d", void_proto());
  expect_parity(scope);
}

// Base{default d}; I extends Base; C implements I (no d); D extends C overrides
// d. (AbstractMutableList.addFirst-like: default on a SUPER-interface.)
TEST_F(VirtualScopesDifferentialTest, DefaultMethodDescendantOverridesSuper) {
  auto scope = create_empty_scope();
  auto* base = make_class(scope, "LBase;", obj(), {},
                          ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_empty_method(base, "d", void_proto());
  auto* i = make_class(scope, "LI;", obj(), {base->get_type()},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "d", void_proto());
  expect_parity(scope);
}

// I{default d}; C implements I (no d); D extends C ALSO implements I (no d).
// (SetWrapper/MutableSetWrapper.spliterator-like: descendant miranda grouped
// into the ancestor's miranda scope.)
TEST_F(VirtualScopesDifferentialTest, DefaultMethodDescendantMiranda) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  make_class(scope, "LD;", c->get_type(), {i->get_type()});
  expect_parity(scope);
}

// ---- Same shapes but with EXTERNAL interfaces (the real-app shape: java.util
// / android.* interfaces with default methods). ----

DexClass* make_ext_intf(Scope& scope,
                        const char* name,
                        const std::vector<DexType*>& supers = {}) {
  auto* i = create_external_class(DexType::make_type(name), obj(), supers,
                                  ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  scope.push_back(i);
  return i;
}

// ext I{default d}; C implements I, no own d. (SetWrapper.forEach single
// miranda)
TEST_F(VirtualScopesDifferentialTest, ExtDefaultNoImpl) {
  auto scope = create_empty_scope();
  auto* i = make_ext_intf(scope, "LEI;");
  create_empty_method(i, "d", void_proto());
  make_class(scope, "LC;", obj(), {i->get_type()});
  expect_parity(scope);
}

// ext I{default d}; C implements I (no d); D extends C overrides d.
// (EmptyActivityLifecycleCallbacks.onActivityPostResumed-like.)
TEST_F(VirtualScopesDifferentialTest, ExtDefaultDescendantOverridesDirect) {
  auto scope = create_empty_scope();
  auto* i = make_ext_intf(scope, "LEI;");
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "d", void_proto());
  expect_parity(scope);
}

// ext Base{default d}; ext I extends Base; C implements I (no d); D extends C
// overrides d. (AbstractMutableList.addFirst-like: default on external SUPER
// interface.)
TEST_F(VirtualScopesDifferentialTest, ExtDefaultDescendantOverridesSuper) {
  auto scope = create_empty_scope();
  auto* base = make_ext_intf(scope, "LEBase;");
  create_empty_method(base, "d", void_proto());
  auto* i = make_ext_intf(scope, "LEI;", {base->get_type()});
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "d", void_proto());
  expect_parity(scope);
}

// ext I{default d}; C implements I (no d); D extends C ALSO implements I (no
// d). (SetWrapper/MutableSetWrapper.spliterator exact shape.)
TEST_F(VirtualScopesDifferentialTest, ExtDefaultDescendantMiranda) {
  auto scope = create_empty_scope();
  auto* i = make_ext_intf(scope, "LEI;");
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  make_class(scope, "LD;", c->get_type(), {i->get_type()});
  expect_parity(scope);
}

// ext I{abstract n}; internal C implements I, no impl.
// (AbstractMutableCollection .iterator shape: unimplemented ABSTRACT method of
// an EXTERNAL interface. Tests whether legacy creates a lone miranda for
// external-abstract like it does for internal-abstract
// PureUnimplementedInterface.)
TEST_F(VirtualScopesDifferentialTest, ExtAbstractNoImpl) {
  auto scope = create_empty_scope();
  auto* i = make_ext_intf(scope, "LEI;");
  create_abstract_method(i, "n", void_proto());
  make_class(scope, "LC;", obj(), {i->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  expect_parity(scope);
}

// Exact kotlin AbstractMutableCollection.iterator shape: ext interface Coll{n};
// EXTERNAL abstract class ExtBase implements Coll and RE-DECLARES n as its own
// abstract method (like java.util.AbstractCollection re-declaring
// Collection.iterator abstract); internal abstract AMC extends ExtBase (no own
// n). n's scope roots at the external ExtBase, so AMC must carry nothing --
// new must not mint a lone miranda at AMC.
TEST_F(VirtualScopesDifferentialTest, ExtBaseRedeclaresAbstractMethod) {
  auto scope = create_empty_scope();
  auto* coll = make_ext_intf(scope, "LEColl;");
  create_abstract_method(coll, "n", void_proto());
  auto* extbase =
      create_external_class(DexType::make_type("LExtBase;"), obj(),
                            {coll->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  scope.push_back(extbase);
  create_abstract_method(extbase, "n", void_proto());
  make_class(scope, "LAMC;", extbase->get_type(), {coll->get_type()},
             ACC_PUBLIC | ACC_ABSTRACT);
  expect_parity(scope);
}

// I{n}; abstract C implements I (no n); M extends C defines n (provider); N
// extends M overrides n (below the provider). Legacy collapses the whole family
// into C's scope [C.miranda, M.n, N.n]; new must include N even though it sits
// below the intermediate provider M. (MutablePropertyReference1 /
// EdgeToEdgeBase shape.)
TEST_F(VirtualScopesDifferentialTest, BelowProviderDescendantGrouped) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* m = make_class(scope, "LM;", c->get_type());
  create_empty_method(m, "n", void_proto());
  auto* n = make_class(scope, "LN;", m->get_type());
  create_empty_method(n, "n", void_proto());
  expect_parity(scope);
}

// Base{n}; Sub extends Base; abstract C implements Base (no n); D extends C
// implements Sub (no n). The miranda scope at C must carry BOTH interfaces
// {Base, Sub} -- D's sub-interface contributes Sub. (BaseDataSource.close =
// {DataSource, HttpDataSource} shape.)
TEST_F(VirtualScopesDifferentialTest, MirandaInterfaceUnionAcrossSubtree) {
  auto scope = create_empty_scope();
  auto* base = make_class(scope, "LBase;", obj(), {},
                          ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(base, "n", void_proto());
  auto* sub = make_class(scope, "LSub;", obj(), {base->get_type()},
                         ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(sub, "n", void_proto()); // Sub re-declares n
  auto* c = make_class(scope, "LC;", obj(), {base->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  make_class(scope, "LD;", c->get_type(), {sub->get_type()});
  expect_parity(scope);
}

// Concrete provider ABOVE an abstract re-declaration (Guava
// ImmutableSet.contains shape): ext Coll{abstract n}; EXTERNAL ExtBase
// implements Coll with CONCRETE n (java.util.AbstractCollection); internal
// abstract MidColl extends ExtBase re-declares n ABSTRACT (Guava
// ImmutableCollection); internal abstract C extends MidColl implements Coll (no
// own n) (ImmutableSet); D extends C defines n. n is implemented by ExtBase
// (external concrete) -> scope roots externally, so nothing internal; new must
// not mint a miranda at C despite MidColl's abstract re-declaration sitting
// between C and the concrete impl.
TEST_F(VirtualScopesDifferentialTest, ConcreteProviderAboveAbstractRedecl) {
  auto scope = create_empty_scope();
  auto* coll = make_ext_intf(scope, "LEColl;");
  create_abstract_method(coll, "n", void_proto());
  auto* extbase = create_external_class(DexType::make_type("LExtBase;"), obj(),
                                        {coll->get_type()});
  scope.push_back(extbase);
  create_empty_method(extbase, "n", void_proto()); // external CONCRETE impl
  auto* midcoll = make_class(scope, "LMidColl;", extbase->get_type(),
                             {coll->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  create_abstract_method(midcoll, "n", void_proto()); // abstract re-declaration
  auto* c = make_class(scope, "LC;", midcoll->get_type(), {coll->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "n", void_proto());
  expect_parity(scope);
}

// I{n}; abstract C implements I with an ABSTRACT own n (so C.n roots a class
// scope); abstract D extends C implements I with no own n (its miranda must
// fold into C.n's scope); E extends C overrides n. C.n scope = [C.n,
// D.n(miranda), E.n]. (Guava ImmutableMap.get shape: abstract root + descendant
// miranda + concrete override; the fold must target the abstract-rooted class
// scope.)
TEST_F(VirtualScopesDifferentialTest, FoldMirandaIntoAbstractClassScopeRoot) {
  auto scope = create_empty_scope();
  auto* i = make_class(scope, "LI;", obj(), {},
                       ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT);
  create_abstract_method(i, "n", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  create_abstract_method(c, "n", void_proto()); // abstract own n -> class root
  auto* d = make_class(scope, "LD;", c->get_type(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  auto* e = make_class(scope, "LE;", c->get_type());
  create_empty_method(e, "n", void_proto());
  (void)d;
  expect_parity(scope);
}

// Generated-shape shape: class extends Object, implements an EXTERNAL interface
// (Parcelable$Creator) with concrete impls of its abstract methods. This
// mirrors PCreatorCreatorShape structurally; if it matches here, the real
// divergence is not structural (it's mid-pass generation state).
TEST_F(VirtualScopesDifferentialTest, ExtInterfaceConcreteImplExtendsObject) {
  auto scope = create_empty_scope();
  auto* creator = make_ext_intf(scope, "LECreator;");
  create_abstract_method(creator, "createFromParcel", void_proto());
  create_abstract_method(creator, "newArray", void_int_proto());
  auto* shape = make_class(scope, "LShape;", obj(), {creator->get_type()});
  create_empty_method(shape, "createFromParcel", void_proto());
  create_empty_method(shape, "newArray", void_int_proto());
  expect_parity(scope);
}

// ESCAPE: C implements an UNRESOLVABLE interface (a DexType with no DexClass,
// like androidx.window.sidecar stubs); C has a lone concrete method m. Legacy
// marks C's methods ESCAPED, so C.m is classified V (not non-virtual) even
// though it's a size-1 no-interface scope; new must match (is_effectively_final
// == false when escaped). Also covers propagation to a subclass D.
TEST_F(VirtualScopesDifferentialTest, EscapeUnknownInterface) {
  auto scope = create_empty_scope();
  auto* unknown = DexType::make_type("LUnknownIntf;"); // no DexClass created
  auto* c = make_class(scope, "LC;", obj(), {unknown});
  create_empty_method(c, "m", void_proto());
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "k", void_proto());
  (void)d;
  expect_parity(scope);
}

// ESCAPE via EXTERNAL ancestor: an external (runtime-jar) class E implements an
// unresolvable interface; the internal mergeable C extends E but does NOT
// itself declare the interface. Legacy builds over build_type_hierarchy(scope),
// which includes E (build_external_hierarchy folds in g_redex externals), seeds
// escape at E, and propagates DOWN to C -- so C.n is classified V, not
// non-virtual. E is intentionally NOT pushed into `scope` (it is
// external-in-context, like a jar class), so seeding escape only from internal
// `scope` would miss it and flip C.n from V to effectively-final. This is the
// reviewer's case.
TEST_F(VirtualScopesDifferentialTest, EscapeViaExternalAncestor) {
  auto scope = create_empty_scope();
  auto* unknown = DexType::make_type("LUnknownExtIntf;"); // no DexClass created
  auto* e =
      create_external_class(DexType::make_type("LExtBase;"), obj(), {unknown});
  create_empty_method(e, "m", void_proto());
  auto* c = make_class(scope, "LC;", e->get_type());
  create_empty_method(c, "n", void_proto());
  (void)c;
  expect_parity(scope);
}

// ext Coll{default d}; ext I extends Coll; C implements I (no d); D extends C
// ALSO implements I (no d). The default d is on the SUPER external interface.
// (AbstractSet/AbstractList exact shape: stream/forEach live on Collection, a
// super-interface of the directly-implemented Set/List.)
TEST_F(VirtualScopesDifferentialTest, ExtSuperDefaultDescendantMiranda) {
  auto scope = create_empty_scope();
  auto* coll = make_ext_intf(scope, "LEColl;");
  create_empty_method(coll, "d", void_proto());
  auto* i = make_ext_intf(scope, "LEI;", {coll->get_type()});
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()});
  make_class(scope, "LD;", c->get_type(), {i->get_type()});
  expect_parity(scope);
}

// Mirrors kotlin AbstractSet exactly: ext Coll{default d}; ext Set extends
// Coll; abstract AbsColl implements Coll (no d); abstract AbsSet extends
// AbsColl implements Set (no d); PSet extends AbsSet (no d). The default d's
// obligation is introduced at the abstract SUPERCLASS AbsColl -- so its scope
// should root there, and AbsSet/PSet should carry nothing for d.
TEST_F(VirtualScopesDifferentialTest, ExtDefaultAbstractSuperclassChain) {
  auto scope = create_empty_scope();
  auto* coll = make_ext_intf(scope, "LEColl;");
  create_empty_method(coll, "d", void_proto());
  auto* set = make_ext_intf(scope, "LESet;", {coll->get_type()});
  auto* abscoll = make_class(scope, "LAbsColl;", obj(), {coll->get_type()},
                             ACC_PUBLIC | ACC_ABSTRACT);
  auto* absset = make_class(scope, "LAbsSet;", abscoll->get_type(),
                            {set->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  make_class(scope, "LPSet;", absset->get_type());
  expect_parity(scope);
}

// Same chain, but the leaf PSet RE-DECLARES the interface (implements Set),
// mirroring PersistentHashSet implements ImmutableSet(->Set). This is the shape
// that originally exposed a re-rooting bug: the leaf's re-declaration gives
// AbsSet a declaring descendant, and an earlier implementation re-rooted d at
// the intermediate AbsSet instead of collapsing the whole family into the
// topmost implementer AbsColl the way legacy does. That is fixed -- both now
// collapse into AbsColl, so this asserts parity (expect_parity).
TEST_F(VirtualScopesDifferentialTest, ExtDefaultReRootIntermediate) {
  auto scope = create_empty_scope();
  auto* coll = make_ext_intf(scope, "LEColl;");
  create_empty_method(coll, "d", void_proto());
  auto* set = make_ext_intf(scope, "LESet;", {coll->get_type()});
  auto* abscoll = make_class(scope, "LAbsColl;", obj(), {coll->get_type()},
                             ACC_PUBLIC | ACC_ABSTRACT);
  auto* absset = make_class(scope, "LAbsSet;", abscoll->get_type(),
                            {set->get_type()}, ACC_PUBLIC | ACC_ABSTRACT);
  make_class(scope, "LPSet;", absset->get_type(), {set->get_type()});
  expect_parity(scope);
}

// ext I{abstract a; default d}; abstract C implements I, defines a (concrete),
// no d; D extends C overrides d. (AbstractMutableList exact shape: abstract
// base implementing the interface's abstract method but inheriting a default
// that a concrete descendant overrides.)
TEST_F(VirtualScopesDifferentialTest, ExtDefaultAbstractBaseWithOwnImpl) {
  auto scope = create_empty_scope();
  auto* i = make_ext_intf(scope, "LEI;");
  create_abstract_method(i, "a", void_proto());
  create_empty_method(i, "d", void_proto());
  auto* c = make_class(scope, "LC;", obj(), {i->get_type()},
                       ACC_PUBLIC | ACC_ABSTRACT);
  create_empty_method(c, "a", void_proto());
  auto* d = make_class(scope, "LD;", c->get_type());
  create_empty_method(d, "d", void_proto());
  expect_parity(scope);
}
