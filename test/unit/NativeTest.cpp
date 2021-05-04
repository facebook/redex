/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "Native.h"
#include "RedexTest.h"

struct NativeTest : public RedexTest {};

TEST_F(NativeTest, testJNIOutputParsing) {
  auto comp_units = native::get_compilation_units(
      boost::filesystem::path(std::getenv("native_jni_output_path")) /
      "JNI_OUTPUT");

  std::unordered_set<std::string> unit_names;
  for (const auto& unit : comp_units) {
    unit_names.emplace(unit.get_name());
  }

  EXPECT_EQ(2, unit_names.size());
  EXPECT_EQ(1, unit_names.count("lib/Dog.cpp"));
  EXPECT_EQ(1, unit_names.count("lib/Hello.cpp"));
}

TEST_F(NativeTest, testBuildingContext) {
  auto path_to_native_results =
      boost::filesystem::path(std::getenv("native_jni_output_path")) /
      "JNI_OUTPUT";

  auto type = DexType::make_type("Lredex/JNIExample;");
  ClassCreator creator(type);
  creator.set_super(type::java_lang_Object());

  DexMethodRef* mref =
      DexMethod::make_method("Lredex/JNIExample;.implemented:()V");
  auto m = mref->make_concrete(ACC_PUBLIC | ACC_NATIVE, false);

  creator.add_method(m);
  auto cls = creator.create();

  Scope java_scope;
  java_scope.push_back(cls);

  auto context =
      native::NativeContext::build(path_to_native_results.string(), java_scope);

  {
    EXPECT_EQ(1, context.name_to_function.size());

    auto func_it =
        context.name_to_function.find("Java_redex_JNIExample_implemented");

    ASSERT_TRUE(func_it != context.name_to_function.end());
    native::Function* func = func_it->second;
    EXPECT_EQ(m, func->get_java_declaration());
  }

  {
    EXPECT_EQ(1, context.java_declaration_to_function.size());
    auto func_it = context.java_declaration_to_function.find(m);
    ASSERT_TRUE(func_it != context.java_declaration_to_function.end());
    native::Function* func = func_it->second;
    EXPECT_EQ("Java_redex_JNIExample_implemented", func->get_name());
  }
}
