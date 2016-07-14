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

#include "Devirtualizer.h"

namespace {

//
// Utility to create classes and methods.
//

/**
 * Create an abstract DexMethod in a given class.
 * It provides the ACC_ABSTRACT flag if one was not sepcified.
 */
DexMethod* create_abstract_method(DexClass* cls, const char* name,
    DexProto* proto, DexAccessFlags access = ACC_PUBLIC) {
  always_assert((access & (ACC_PRIVATE | ACC_STATIC)) == 0);
  access = access | ACC_ABSTRACT;
  auto method = DexMethod::make_method(
      cls->get_type(), DexString::make_string(name), proto);
  method->make_concrete(access, nullptr, true);
  cls->add_method(method);
  return method;
}

/**
 * Create a DexMethod in a given class.
 * Generate a body with a simple return for the method.
 */
DexMethod* create_method(DexClass* cls, const char* name, DexProto* proto,
    DexAccessFlags access = ACC_PUBLIC) {
  if ((access & ACC_ABSTRACT) != 0) {
    return create_abstract_method(cls, name, proto, access);
  }
  MethodCreator mcreator(cls->get_type(),
      DexString::make_string(name), proto, access);
  auto main_block = mcreator.get_main_block();
  auto rtype = proto->get_rtype();
  if (rtype == get_void_type()) {
    main_block->ret_void();
  } else {
    auto null_loc = mcreator.make_local(rtype);
    main_block->load_null(null_loc);
    main_block->ret(null_loc);
  }
  auto method = mcreator.create();
  cls->add_method(method);
  return method;
}

/**
 * Create a DexClass.
 */
DexClass* create_class(DexType* type, DexType* super,
    std::vector<DexType*> interfaces, DexAccessFlags access = ACC_PUBLIC,
    bool external = false) {
  ClassCreator creator(type);
  creator.set_access(access);
  if (external) creator.set_external();
  if (super == nullptr) super = get_object_type();
  creator.set_super(super);
  for (const auto& interface : interfaces) {
    creator.add_interface(interface);
  }
  return creator.create();
}

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
  std::vector<DexClass*> scope;
  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_class(a_t, obj_t, {});
  create_method(a_cls, "final1", void_void);
  create_method(a_cls, "final2", void_void);
  scope.push_back(a_cls);

  return scope;
}

/**
 * Make a scope with:
 * interface Interf { void intf_meth1(); void intf_meth2(); }
 * abstract class A implements Interf { void final1() {} void intf_meth1() {} }
 */
std::vector<DexClass*> create_scope_2() {
  std::vector<DexClass*> scope;
  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls = create_class(
      interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_class(a_t, obj_t, {interf_t}, ACC_ABSTRACT);
  create_method(a_cls, "final1", void_void);
  create_method(a_cls, "intf_meth1", void_void);
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

  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be already defined in scope");

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_class(b_t, a_t, {});
  create_method(b_cls, "final2", void_void);
  create_method(b_cls, "intf_meth2", void_void);
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
  std::vector<DexClass*> scope;
  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls = create_class(
      interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_class(a_t, obj_t, {});
  create_method(a_cls, "final1", void_void);
  create_method(a_cls, "intf_meth1", void_void);
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_class(b_t, a_t, {interf_t});
  create_method(b_cls, "intf_meth2", void_void);
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
  std::vector<DexClass*> scope;
  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);

  auto interf_t = DexType::make_type("LInterf;");
  auto interf_cls = create_class(
      interf_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf_cls, "intf_meth1", void_void, ACC_PUBLIC);
  create_abstract_method(interf_cls, "intf_meth2", void_void, ACC_PUBLIC);
  scope.push_back(interf_cls);

  auto a_t = DexType::make_type("LA;");
  auto a_cls = create_class(a_t, obj_t, {});
  create_method(a_cls, "override1", void_void);
  create_method(a_cls, "intf_meth1", void_void);
  scope.push_back(a_cls);

  auto b_t = DexType::make_type("LB;");
  auto b_cls = create_class(b_t, a_t, {interf_t});
  create_method(b_cls, "override1", void_void);
  create_method(b_cls, "final1", void_void);
  create_method(b_cls, "intf_meth2", void_void);
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

  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");
  auto interf_t = DexType::get_type("LInterf;");
  always_assert_log(a_t != nullptr,
      "interface Interf must be defined in scope already");

  auto c_t = DexType::make_type("LC;");
  auto c_cls = create_class(c_t, a_t, {interf_t});
  create_method(c_cls, "final1", void_void);
  create_method(c_cls, "intf_meth2", void_void);
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

  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto d_t = DexType::make_type("LD;");
  auto d_cls = create_class(d_t, a_t, {});
  create_method(d_cls, "override1", void_void);
  scope.push_back(d_cls);

  auto e_t = DexType::make_type("LE;");
  auto e_cls = create_class(e_t, a_t, {});
  create_method(e_cls, "final1", void_void);
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

  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto int_t = get_int_type();
  auto void_args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(void_t, void_args);
  auto int_args = DexTypeList::make_type_list({int_t});
  auto int_void = DexProto::make_proto(void_t, int_args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_class(f_t, a_t, {});
  create_method(f_cls, "final1", void_void);
  create_method(f_cls, "intf_meth1", int_void);
  scope.push_back(f_cls);

  auto g_t = DexType::make_type("LG;");
  auto g_cls = create_class(g_t, f_t, {});
  create_method(g_cls, "intf_meth2", int_void);
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

  auto obj_t = get_object_type();
  auto void_t = get_void_type();
  auto int_t = get_int_type();
  auto int_args = DexTypeList::make_type_list({int_t});
  auto int_void = DexProto::make_proto(void_t, int_args);
  auto a_t = DexType::get_type("LA;");
  always_assert_log(a_t != nullptr, "class A must be defined in scope already");

  auto interf1_t = DexType::make_type("LInterf1;");
  auto interf1_cls = create_class(
      interf1_t, obj_t, {}, ACC_PUBLIC | ACC_INTERFACE);
  create_abstract_method(interf1_cls, "intf_meth1", int_void, ACC_PUBLIC);
  scope.push_back(interf1_cls);

  auto f_t = DexType::make_type("LF;");
  auto f_cls = create_class(f_t, a_t, {interf1_t});
  create_method(f_cls, "intf_meth1", int_void);
  scope.push_back(f_cls);

  return scope;
}

//
// Assert utilities for tests
//
bool check_names(const std::vector<DexMethod*>& methods,
    const std::vector<DexString*>& names) {
  for (const auto& method : methods) {
    if (std::find(names.begin(), names.end(), method->get_name()) ==
        names.end()) {
      return false;
    }
  }
  return true;
}

bool check_classes(const std::vector<DexMethod*>& methods,
    const std::vector<DexType*>& types) {
  for (const auto& method : methods) {
    if (std::find(types.begin(), types.end(), method->get_class()) ==
        types.end()) {
      return false;
    }
  }
  return true;
}

}

//
// Tests
//

/*
TEST(OneClass2Finals, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_1();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 2);
  if (strcmp(methods[0]->get_name()->c_str(), "final1") == 0) {
    EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
    EXPECT_STREQ(methods[1]->get_name()->c_str(), "final2");
  } else {
    EXPECT_STREQ(methods[1]->get_name()->c_str(), "final1");
    EXPECT_STREQ(methods[0]->get_name()->c_str(), "final2");
  }
  EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LA;");
  EXPECT_STREQ(methods[1]->get_class()->get_name()->c_str(), "LA;");
  delete g_redex;
}

TEST(AbstractClassInterface1Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_2();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 1);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LA;");
  delete g_redex;
}

TEST(InterfaceClassInheritance2Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_3();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 2);
  if (strcmp(methods[0]->get_name()->c_str(), "final1") == 0) {
    EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
    EXPECT_STREQ(methods[1]->get_name()->c_str(), "final2");
    EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LA;");
    EXPECT_STREQ(methods[1]->get_class()->get_name()->c_str(), "LB;");
  } else {
    EXPECT_STREQ(methods[0]->get_name()->c_str(), "final2");
    EXPECT_STREQ(methods[1]->get_name()->c_str(), "final1");
    EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LB;");
    EXPECT_STREQ(methods[1]->get_class()->get_name()->c_str(), "LA;");
  }
  delete g_redex;
}

TEST(InterfaceWithImplInBase1Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_4();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 1);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LA;");
  delete g_redex;
}

TEST(InterfaceWithImplInBaseAndOverride1Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_5();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 1);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LB;");
  delete g_redex;
}

TEST(InterfaceWithImplInBase2Classes2Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_6();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 2);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[1]->get_name()->c_str(), "final1");
  if (strcmp(methods[0]->get_class()->get_name()->c_str(), "LB;") == 0) {
    EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LB;");
    EXPECT_STREQ(methods[1]->get_class()->get_name()->c_str(), "LC;");
  } else {
    EXPECT_STREQ(methods[0]->get_class()->get_name()->c_str(), "LC;");
    EXPECT_STREQ(methods[1]->get_class()->get_name()->c_str(), "LB;");
  }
  delete g_redex;
}

TEST(InterfaceWithImplInBaseMultipleClasses3Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_7();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 3);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[1]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[2]->get_name()->c_str(), "final1");
  std::vector<DexType*> types = {
      DexType::get_type("LB;"),
      DexType::get_type("LC;"),
      DexType::get_type("LE;") };
  EXPECT_TRUE(check_classes(methods, types));
  delete g_redex;
}

TEST(InterfaceWithImplInBaseMultipleClassesAndOverloads6Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_8();
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 6);
  std::vector<DexString*> names = {
      DexString::get_string("final1"),
      DexString::get_string("intf_meth1"),
      DexString::get_string("intf_meth2") };
  EXPECT_TRUE(check_names(methods, names));
  std::vector<DexType*> types = {
      DexType::get_type("LB;"),
      DexType::get_type("LC;"),
      DexType::get_type("LE;"),
      DexType::get_type("LF;"),
      DexType::get_type("LG;") };
  EXPECT_TRUE(check_classes(methods, types));
  delete g_redex;
}
*/

TEST(InterfacesWithImplInBaseMultipleClassesAndOverloads3Final, empty) {
  g_redex = new RedexContext();
  std::vector<DexClass*> scope = create_scope_9();
  // std::reverse(scope.begin(), scope.end());
  auto methods = devirtualize(scope);
  EXPECT_EQ(methods.size(), 3);
  EXPECT_STREQ(methods[0]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[1]->get_name()->c_str(), "final1");
  EXPECT_STREQ(methods[2]->get_name()->c_str(), "final1");
  std::vector<DexType*> types = {
      DexType::get_type("LB;"),
      DexType::get_type("LC;"),
      DexType::get_type("LE;") };
  EXPECT_TRUE(check_classes(methods, types));
  delete g_redex;
}
