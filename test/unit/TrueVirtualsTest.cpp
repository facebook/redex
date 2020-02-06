/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include "MethodOverrideGraph.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

namespace mog = method_override_graph;

namespace {

//
// Utility to create classes and methods.
//

//
// Scope creation for the different tests.
// they are defined here so we can compose the functions as needed.
// Keep that in mind if making changes.
//

/**
 * Make a scope with:
 * class A { void final1() {} void final2() {} }
 */
std::vector<DexClass*> create_scope_1() {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  create_empty_method(a_cls, "final1", void_void);
  create_empty_method(a_cls, "final2", void_void);
  scope.push_back(a_cls);

  return scope;
}

/**
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * abstract class A implements Interf { void final1() {} void intf_meth1() {} }
 */
std::vector<DexClass*> create_scope_2() {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls =
      create_internal_class(interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {interf_t}, ACC_ABSTRACT);
  create_empty_method(a_cls, "final1", void_void);
  create_empty_method(a_cls, "intf_meth1", void_void);
  scope.push_back(a_cls);

  return scope;
}

/**
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * abstract class A implements Interf { void final1() {} void intf_meth1() {} }
 * class B extends A { void final2() {} void intf_meth2() {} }
 */
std::vector<DexClass*> create_scope_3() {
  auto scope = create_scope_2();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be already defined in scope");

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, a_t, {});
  create_empty_method(b_cls, "final2", void_void);
  create_empty_method(b_cls, "intf_meth2", void_void);
  scope.push_back(b_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * class A { void final1() {} void intf_meth1() {} }
 * class B extends A implements Interf { void intf_meth2() {} }
 */
std::vector<DexClass*> create_scope_4() {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls =
      create_internal_class(interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  create_empty_method(a_cls, "final1", void_void);
  create_empty_method(a_cls, "intf_meth1", void_void);
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, a_t, {interf_t});
  create_empty_method(b_cls, "intf_meth2", void_void);
  scope.push_back(b_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * class A { void override1() {} void intf_meth1() {} }
 * class B extends A implements Interf {
 *    void override1() {} void final1() {} void intf_meth2() {} }
 */
std::vector<DexClass*> create_scope_5() {
  std::vector<DexClass*> scope = create_empty_scope();
  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls =
      create_internal_class(interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_internal_class(a_t, obj_t, {});
  create_empty_method(a_cls, "override1", void_void);
  create_empty_method(a_cls, "intf_meth1", void_void);
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_internal_class(b_t, a_t, {interf_t});
  create_empty_method(b_cls, "override1", void_void);
  create_empty_method(b_cls, "final1", void_void);
  create_empty_method(b_cls, "intf_meth2", void_void);
  scope.push_back(b_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * class A { void override1() {} void intf_meth1() {} }
 * class B extends A implements Interf {
 *    void override1() {} void final1() {} void intf_meth2() {} }
 * class C extends A implements Interf { void final1() {} void intf_meth2() {} }
 */
std::vector<DexClass*> create_scope_6() {
  std::vector<DexClass*> scope = create_scope_5();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");
  auto interf_t = DexType::get_type("LInterf;");
  always_assert_log(a_t != nullptr,
                    "interface Interf must be defined in scope already");

  auto c_t = DexType::make_type("LC;");
  auto c_cls = create_internal_class(c_t, a_t, {interf_t});
  create_empty_method(c_cls, "final1", void_void);
  create_empty_method(c_cls, "intf_meth2", void_void);
  scope.push_back(c_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * class A { void override1() {} void intf_meth1() {} }
 * class B extends A implements Interf {
 *    void override1() {} void final1() {} void intf_meth2() {} }
 * class C extends A implements Interf { void final1() {} void intf_meth2() {} }
 * class D extends A { void override1() {} }
 * class E extends A { void final1() {} }
 */
std::vector<DexClass*> create_scope_7() {
  std::vector<DexClass*> scope = create_scope_6();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto d_t = DexType::make_type("LD;");
  auto d_cls = create_internal_class(d_t, a_t, {});
  create_empty_method(d_cls, "override1", void_void);
  scope.push_back(d_cls);

  auto e_t = DexType::make_type("LE;");
  auto e_cls = create_internal_class(e_t, a_t, {});
  create_empty_method(e_cls, "final1", void_void);
  scope.push_back(e_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * class A { void override1() {} void intf_meth1() {} }
 * class B extends A implements Interf {
 *    void override1() {} void final1() {} void intf_meth2() {} }
 * class C extends A implements Interf { void final1() {} void intf_meth2() {} }
 * class D extends A { void override1() {} }
 * class E extends A { void final1() {} }
 * class F extends A { void final1() {} void intf_meth1(int) {} }
 * class G extends F { void intf_meth2(int) {} }
 * the intf_meth* in F and G are not interface methods but overloads.
 */
std::vector<DexClass*> create_scope_8() {
  std::vector<DexClass*> scope = create_scope_7();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto void_args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, void_args);
  auto int_args = DexTypeList::make_type_list({int_t});
  auto int_void = DexProto::make_proto(void_t, int_args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_internal_class(f_t, a_t, {});
  create_empty_method(f_cls, "final1", void_void);
  create_empty_method(f_cls, "intf_meth1", int_void);
  scope.push_back(f_cls);

  auto g_t = DexType::make_type("LG;");
  auto g_cls = create_internal_class(g_t, f_t, {});
  create_empty_method(g_cls, "intf_meth2", int_void);
  scope.push_back(g_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * interface Interf1 { void intf_meth2(); }
 * class A { void override1() {} void intf_meth1() {} }
 * class B extends A implements Interf {
 *    void override1() {} void final1() {} void intf_meth2() {} }
 * class C extends A implements Interf { void final1() {} void intf_meth2() {} }
 * class D extends A { void override1() {} }
 * class E extends A { void final1() {} }
 * class F extends A implements Interf1 { void intf_meth2() {} }
 */
std::vector<DexClass*> create_scope_9() {
  std::vector<DexClass*> scope = create_scope_7();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto int_args = DexTypeList::make_type_list({int_t});
  auto int_void = DexProto::make_proto(void_t, int_args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto interf1_t = DexType::make_type("LInterf1;");
  auto interf1_cls =
      create_internal_class(interf1_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf1_cls, "intf_meth1", int_void, ACC_PUBLIC);
  scope.push_back(interf1_cls);

  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_internal_class(f_t, a_t, {interf1_t});
  create_empty_method(f_cls, "intf_meth1", int_void);
  scope.push_back(f_cls);

  return scope;
}

/*
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * interface Interf1 { void intf_meth2(); }
 * class A { void override1() {} void final1() {} }
 * class AA extends A { void override1() {} void intf_meth1() {} void
 * final1(int) {} } class AAA extends AA implements Interf { void final2() {}
 * void intf_meth2() {} } class AAB extends AA implements Interf { void final2()
 * {} } class AABA extends AAB { void override1() void intf_meth2() {} } class
 * AB extends A { void override1() {} void final1(int) {} } class ABA extends AB
 * implements Interf { void override1() {} void intf_meth1() {} void final2() {}
 * } class ABAA extends ABA implements Interf1 { void intf_meth2() {} void
 * final1(int) {} } class ABAB extends AB { void intf_meth2() {} void
 * final1(int) {} }
 */
std::vector<DexClass*> create_scope_10() {
  std::vector<DexClass*> scope = create_empty_scope();

  auto obj_t = type::java_lang_Object();
  auto void_t = type::_void();
  auto int_t = type::_int();
  auto no_args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, no_args);
  auto int_args = DexTypeList::make_type_list({int_t});
  auto int_void = DexProto::make_proto(void_t, int_args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf1_t = DexType::make_type("LInterf1;");
  auto a_t = DexType::make_type("LA;");
  auto aa_t = DexType::make_type("LAA;");
  auto aaa_t = DexType::make_type("LAAA;");
  auto aab_t = DexType::make_type("LAAB;");
  auto aaba_t = DexType::make_type("LAABA;");
  auto ab_t = DexType::make_type("LAB;");
  auto aba_t = DexType::make_type("LABA;");
  auto abaa_t = DexType::make_type("LABAA;");
  auto abab_t = DexType::make_type("LABAB;");

  // push interfaces
  auto interf_cls =
      create_internal_class(interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);
  auto interf1_cls =
      create_internal_class(interf1_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf1_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf1_cls);

  auto a_cls = create_internal_class(a_t, obj_t, {});
  create_empty_method(a_cls, "override1", void_void);
  create_empty_method(a_cls, "final1", void_void);
  scope.push_back(a_cls);
  auto aa_cls = create_internal_class(aa_t, a_t, {});
  create_empty_method(aa_cls, "override1", void_void);
  create_empty_method(aa_cls, "intf_meth1", void_void);
  create_empty_method(aa_cls, "final1", int_void);
  scope.push_back(aa_cls);
  auto aaa_cls = create_internal_class(aaa_t, aa_t, {interf_t});
  create_empty_method(aaa_cls, "final2", void_void);
  create_empty_method(aaa_cls, "intf_meth2", void_void);
  scope.push_back(aaa_cls);
  auto aab_cls = create_internal_class(aab_t, aa_t, {interf_t});
  create_empty_method(aab_cls, "final2", void_void);
  scope.push_back(aab_cls);
  auto aaba_cls = create_internal_class(aaba_t, aab_t, {});
  create_empty_method(aaba_cls, "override1", void_void);
  create_empty_method(aaba_cls, "intf_meth2", void_void);
  scope.push_back(aaba_cls);
  auto ab_cls = create_internal_class(ab_t, a_t, {});
  create_empty_method(ab_cls, "override1", void_void);
  create_empty_method(ab_cls, "final1", int_void);
  scope.push_back(ab_cls);
  auto aba_cls = create_internal_class(aba_t, ab_t, {interf_t});
  create_empty_method(aba_cls, "override1", void_void);
  create_empty_method(aba_cls, "intf_meth1", void_void);
  create_empty_method(aba_cls, "final2", void_void);
  scope.push_back(aba_cls);
  auto abaa_cls = create_internal_class(abaa_t, aba_t, {interf1_t});
  create_empty_method(abaa_cls, "intf_meth2", void_void);
  create_empty_method(abaa_cls, "final1", int_void);
  scope.push_back(abaa_cls);
  auto abab_cls = create_internal_class(abab_t, aba_t, {});
  create_empty_method(abab_cls, "intf_meth2", void_void);
  create_empty_method(abab_cls, "final1", int_void);
  scope.push_back(abab_cls);

  return scope;
}

//
// Utilities for tests
//

std::unordered_set<std::string> get_method_names(
    const std::unordered_set<DexMethod*>& methods) {
  std::unordered_set<std::string> result;
  for (auto* method : methods) {
    result.emplace(show(method));
  }
  return result;
}

} // namespace

//
// Tests
//

class DevirtualizerTest : public RedexTest {};

TEST_F(DevirtualizerTest, OneClass2Finals) {
  std::vector<DexClass*> scope = create_scope_1();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(
      get_method_names(methods),
      ::testing::UnorderedElementsAre("LA;.final1:()V", "LA;.final2:()V"));
}

TEST_F(DevirtualizerTest, AbstractClassInterface1Final) {
  std::vector<DexClass*> scope = create_scope_2();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre("LA;.final1:()V"));
}

TEST_F(DevirtualizerTest, InterfaceClassInheritance2Final) {
  std::vector<DexClass*> scope = create_scope_3();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(
      get_method_names(methods),
      ::testing::UnorderedElementsAre("LA;.final1:()V", "LB;.final2:()V"));
}

TEST_F(DevirtualizerTest, InterfaceWithImplInBase1Final) {
  std::vector<DexClass*> scope = create_scope_4();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre("LA;.final1:()V"));
}

TEST_F(DevirtualizerTest, InterfaceWithImplInBaseAndOverride1Final) {
  std::vector<DexClass*> scope = create_scope_5();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre("LB;.final1:()V"));
}

TEST_F(DevirtualizerTest, InterfaceWithImplInBase2Classes2Final) {
  std::vector<DexClass*> scope = create_scope_6();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(
      get_method_names(methods),
      ::testing::UnorderedElementsAre("LB;.final1:()V", "LC;.final1:()V"));
}

TEST_F(DevirtualizerTest, InterfaceWithImplInBaseMultipleClasses3Final) {
  std::vector<DexClass*> scope = create_scope_7();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre(
                  "LB;.final1:()V", "LC;.final1:()V", "LE;.final1:()V"));
}

TEST_F(DevirtualizerTest,
       InterfaceWithImplInBaseMultipleClassesAndOverloads6Final) {
  std::vector<DexClass*> scope = create_scope_8();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre(
                  "LB;.final1:()V", "LC;.final1:()V", "LE;.final1:()V",
                  "LF;.final1:()V", "LF;.intf_meth1:(I)V",
                  "LG;.intf_meth2:(I)V"));
}

TEST_F(DevirtualizerTest,
       InterfacesWithImplInBaseMultipleClassesAndOverloads3Final) {
  std::vector<DexClass*> scope = create_scope_9();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre(
                  "LB;.final1:()V", "LC;.final1:()V", "LE;.final1:()V"));
}

TEST_F(DevirtualizerTest, GenericRichHierarchy) {
  std::vector<DexClass*> scope = create_scope_10();
  auto methods = mog::get_non_true_virtuals(scope);
  EXPECT_THAT(get_method_names(methods),
              ::testing::UnorderedElementsAre(
                  "LA;.final1:()V", "LABA;.final2:()V", "LAA;.final1:(I)V",
                  "LAAB;.final2:()V", "LAAA;.final2:()V"));
}
