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
