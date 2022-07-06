/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/filesystem.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "Native.h"
#include "RedexTest.h"

struct NativeTest : public RedexTest {};

TEST_F(NativeTest, testJNIOutputParsing) {
  auto libs = native::get_so_libraries(
      boost::filesystem::path(std::getenv("native_jni_output_path")) /
      "JNI_OUTPUT");

  std::unordered_set<std::string> lib_names;
  for (const auto& lib : libs) {
    lib_names.emplace(lib.get_name());
  }

  EXPECT_THAT(lib_names,
              ::testing::UnorderedElementsAre("libhello.so", "libempty.so"));
}

TEST_F(NativeTest, testBuildingContext) {
  auto path_to_native_results =
      boost::filesystem::path(std::getenv("native_jni_output_path")) /
      "JNI_OUTPUT";

  auto type = DexType::make_type("Lredex/JNIExample;");
  ClassCreator creator(type);
  creator.set_super(type::java_lang_Object());

  DexMethodRef* init_hybrid_ref =
      DexMethod::make_method("Lredex/JNIExample;.initHybrid:()V");
  auto init_hybrid =
      init_hybrid_ref->make_concrete(ACC_PUBLIC | ACC_NATIVE, false);

  creator.add_method(init_hybrid);

  DexMethodRef* foo_ref =
      DexMethod::make_method("Lredex/JNIExample;.foo:(II)I");
  auto foo = foo_ref->make_concrete(ACC_PUBLIC | ACC_NATIVE, false);

  creator.add_method(foo);

  auto cls = creator.create();

  Scope java_scope;
  java_scope.push_back(cls);

  auto context =
      native::NativeContext::build(path_to_native_results.string(), java_scope);

  {
    EXPECT_EQ(2, context.so_libraries.size());

    auto java_decl_of =
        [&](const std::string& native_func) -> std::unordered_set<DexMethod*> {
      for (auto& lib : context.so_libraries) {
        auto f = lib.get_function(native_func);
        if (f) {
          return f->get_java_declarations();
        }
      }
      return {};
    };

    EXPECT_EQ(init_hybrid, *java_decl_of("init_hybrid_impl").begin());
    EXPECT_EQ(foo, *java_decl_of("foo_impl").begin());
  }

  {
    EXPECT_EQ(2, context.java_declaration_to_function.size());

    auto native_impl_of = [&](DexMethod* method) {
      return context.java_declaration_to_function.at(method)->get_name();
    };

    EXPECT_EQ("init_hybrid_impl", native_impl_of(init_hybrid));
    EXPECT_EQ("foo_impl", native_impl_of(foo));
  }
}
