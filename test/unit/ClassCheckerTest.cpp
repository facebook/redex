/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassChecker.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "ScopeHelper.h"

class ClassVCheckerTest : public RedexTest {};

TEST_F(ClassVCheckerTest, testNonAbstractClassWithAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls =
      create_internal_class(a_type, type::java_lang_Object(), {}, ACC_PUBLIC);
  create_abstract_method(a_cls, "m", void_void);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_TRUE(checker.fail());
}

TEST_F(ClassVCheckerTest, testNonAbstractClassWithNonAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls =
      create_internal_class(a_type, type::java_lang_Object(), {}, ACC_PUBLIC);
  create_empty_method(a_cls, "m", void_void, ACC_PUBLIC);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testAbstractClassWithAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls = create_internal_class(a_type, type::java_lang_Object(), {},
                                          ACC_PUBLIC | ACC_ABSTRACT);
  create_abstract_method(a_cls, "m", void_void);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testInterfaceClassWithAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls = create_internal_class(a_type, type::java_lang_Object(), {},
                                          ACC_INTERFACE);
  create_abstract_method(a_cls, "m", void_void);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testAbstractClassWithNonAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls =
      create_internal_class(a_type, type::java_lang_Object(), {}, ACC_ABSTRACT);
  create_empty_method(a_cls, "m", void_void);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testInterfaceClassWithNonAbstractMethods) {
  Scope scope = create_empty_scope();
  auto void_t = type::_void();
  auto void_void =
      DexProto::make_proto(void_t, DexTypeList::make_type_list({}));

  DexType* a_type = DexType::make_type("LA;");
  DexClass* a_cls = create_internal_class(a_type, type::java_lang_Object(), {},
                                          ACC_INTERFACE);
  create_empty_method(a_cls, "m", void_void);

  scope.push_back(a_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

namespace {
// Make a super class A with a method called foo, of given proto and access, and
// a subclass B also with a method called foo of given proto and access.
Scope make_scope_with_a_foo_b_foo(const std::string& a_package,
                                  DexProto* a_foo_proto,
                                  DexAccessFlags a_foo_access,
                                  const std::string& b_package,
                                  DexProto* b_foo_proto,
                                  DexAccessFlags b_foo_access) {
  Scope scope = create_empty_scope();

  DexType* a_type = DexType::make_type(a_package + "A;");
  DexClass* a_cls =
      create_internal_class(a_type, type::java_lang_Object(), {}, ACC_PUBLIC);

  auto a_foo = create_empty_method(a_cls, "foo", a_foo_proto, a_foo_access);
  always_assert(a_foo->is_virtual());

  DexType* b_type = DexType::make_type(b_package + "B;");
  DexClass* b_cls = create_internal_class(b_type, a_type, {}, ACC_PUBLIC);
  auto b_foo = create_empty_method(b_cls, "foo", b_foo_proto, b_foo_access);
  always_assert(b_foo->is_virtual());

  scope.push_back(a_cls);
  scope.push_back(b_cls);
  return scope;
}

Scope make_scope_with_a_foo_b_foo(DexProto* a_foo_proto,
                                  DexAccessFlags a_foo_access,
                                  DexProto* b_foo_proto,
                                  DexAccessFlags b_foo_access) {
  return make_scope_with_a_foo_b_foo("Lredex/", a_foo_proto, a_foo_access,
                                     "Lredex/", b_foo_proto, b_foo_access);
}
} // namespace

TEST_F(ClassVCheckerTest, testFinalMethodNotInSubclassPasses) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto int_return_int = DexProto::make_proto(
      type::_int(), DexTypeList::make_type_list({type::_int()}));
  // A and B are in the same package, both defining foo() with different return
  // value. No problem.
  auto scope = make_scope_with_a_foo_b_foo(
      int_return_void, ACC_PUBLIC | ACC_FINAL, int_return_int, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testFinalMethodInSubclassFails) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  // A and B are in the same package, both defining foo() with same proto.
  // A.foo() is final, which should be disallowed.
  auto scope = make_scope_with_a_foo_b_foo(
      int_return_void, ACC_PUBLIC | ACC_FINAL, int_return_void, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_TRUE(checker.fail());
  std::cerr << checker.print_failed_classes().str() << std::endl;
}

TEST_F(ClassVCheckerTest, testProtectedFinalMethodInSubclassFails) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  // Same as above, but A.foo() is protected, still should be disallowed.
  auto scope = make_scope_with_a_foo_b_foo(
      int_return_void, ACC_PROTECTED | ACC_FINAL, int_return_void, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_TRUE(checker.fail());
  std::cerr << checker.print_failed_classes().str() << std::endl;
}

TEST_F(ClassVCheckerTest, testDefaultAccessFinalMethodInSubclassSamePkgFails) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  // Same as above, but A.foo() is default access, still should be disallowed.
  auto scope = make_scope_with_a_foo_b_foo(int_return_void, ACC_FINAL,
                                           int_return_void, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_TRUE(checker.fail());
  std::cerr << checker.print_failed_classes().str() << std::endl;
}

TEST_F(ClassVCheckerTest, testDefaultAccessFinalMethodInSubclassOtherPkgPass) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  // A.foo() is final, package-private, and B.foo() has same signature but in a
  // different package, should be fine.
  auto scope =
      make_scope_with_a_foo_b_foo("Lredex/", int_return_void, ACC_FINAL,
                                  "Lother/", int_return_void, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

TEST_F(ClassVCheckerTest, testNonFinalOverrideInSubclassPasses) {
  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  // A and B are in the same package, both defining foo() with same proto.
  // A.foo() is not final, no problem.
  auto scope = make_scope_with_a_foo_b_foo(int_return_void, ACC_PUBLIC,
                                           int_return_void, ACC_PUBLIC);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_FALSE(checker.fail());
}

// This following example would not compile if written as source code
// (P1203397896) but is represented in a test case for thoroughness.
TEST_F(ClassVCheckerTest, testVeryFunnyBusiness) {
  Scope scope = create_empty_scope();

  auto int_return_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  DexAccessFlags def_access{};

  DexType* a_type = DexType::make_type("Lredex/A;");
  DexClass* a_cls =
      create_internal_class(a_type, type::java_lang_Object(), {}, ACC_PUBLIC);

  auto a_foo = create_empty_method(a_cls, "foo", int_return_void, ACC_FINAL);
  always_assert(a_foo->is_virtual());

  DexType* b_type = DexType::make_type("Lother/B;");
  DexClass* b_cls = create_internal_class(b_type, a_type, {}, ACC_PUBLIC);
  auto b_foo = create_empty_method(b_cls, "foo", int_return_void, def_access);
  always_assert(b_foo->is_virtual());

  DexType* c_type = DexType::make_type("Lredex/C;");
  DexClass* c_cls = create_internal_class(c_type, b_type, {}, ACC_PUBLIC);
  auto c_foo = create_empty_method(c_cls, "foo", int_return_void, def_access);
  always_assert(c_foo->is_virtual());

  scope.push_back(a_cls);
  scope.push_back(b_cls);
  scope.push_back(c_cls);

  ClassChecker checker;
  checker.run(scope);
  EXPECT_TRUE(checker.fail());
}
