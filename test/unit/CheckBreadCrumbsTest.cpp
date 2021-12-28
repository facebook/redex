/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CheckBreadcrumbs.h"

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"

// Create the following hierarchy
//
// package CheckBreadCrumbs;
//
// class no_modifiers {
//   static int no_modifier_field;
//   static void no_modifier_fun() {
//   }
// }
// class A {
//   static public int a_pub_field;
//   static protected int a_pro_field;
//   static private int a_pri_field;
//   static public void a_pub_fun() {
//   }
//   static protected void a_pro_fun() {
//   }
//   static private void a_pri_func() {
//   }
// }
//
// class B extends A {
//   public int call_a_pub_field() {
//     return a_pub_field;
//   }
//   public int call_a_pro_field() {
//     return a_pro_field;
//   }
//   public int call_a_pri_field() {
//     return a_pri_field;
//   };
//   public void call_a_pub_fun() {
//     a_pub_fun();
//   }
//   public void call_a_pro_fun() {
//     a_pro_fun();
//   }
//   public void call_a_pri_func() {
//     a_pri_func();
//   }
// }
//

namespace {

DexField* make_field_def(DexType* cls,
                         const char* name,
                         DexType* type,
                         DexAccessFlags access = ACC_PUBLIC,
                         bool external = false) {
  auto field = static_cast<DexField*>(
      DexField::make_field(cls, DexString::make_string(name), type));
  if (external) {
    field->set_access(access);
    field->set_external();
  } else {
    field->make_concrete(access);
  }
  return field;
}

std::vector<DexMethod*> call_a_fields_and_methods_methods() {
  auto call_a_pub_field = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_pub_field:()I"
      (
        (sget "LB;.a_pub_field:I")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");

  auto call_a_pro_field = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_proc_field:()I"
      (
        (sget "LB;.a_pro_field:I")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");

  auto call_a_pri_field = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_pri_field:()I"
      (
        (sget "LB;.a_pri_field:I")
        (move-result-pseudo v0)
        (return v0)
      )
    )
  )");

  auto call_a_pub_fun = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_pub_fun:()V"
      (
        (invoke-static () "LA;.a_pub_fun:()V")
        (return-void)
      )
    )
  )");

  auto call_a_pro_fun = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_pro_fun:()V"
      (
        (invoke-static () "LA;.a_pro_fun:()V")
        (return-void)
      )
    )
  )");

  auto call_a_pri_fun = assembler::method_from_string(R"(
    (method (public) "LB;.call_a_pri_fun:()V"
      (
        (invoke-static () "LA;.a_pri_fun:()V")
        (return-void)
      )
    )
  )");

  return std::vector<DexMethod*>{call_a_pub_field, call_a_pro_field,
                                 call_a_pri_field, call_a_pub_fun,
                                 call_a_pro_fun,   call_a_pri_fun};
}

DexClass* create_class(DexType* type,
                       DexType* super,
                       std::vector<DexMethod*>& methods,
                       std::vector<DexField*>& fields,
                       DexAccessFlags access = ACC_PUBLIC,
                       bool external = false) {
  ClassCreator creator(type);
  creator.set_access(access);
  if (external) creator.set_external();
  if (super != nullptr) {
    creator.set_super(super);
  }
  for (const auto& meth : methods) {
    creator.add_method(meth);
  }
  for (const auto& field : fields) {
    creator.add_field(field);
  }
  return creator.create();
}

DexClass* create_class_A() {
  auto int_t = type::_int();
  auto a_t = DexType::make_type("LA;");

  std::vector<DexField*> a_fields{
      make_field_def(a_t, "a_pub_field", int_t, ACC_PUBLIC | ACC_STATIC),
      make_field_def(a_t, "a_pro_field", int_t, ACC_PROTECTED | ACC_STATIC),
      make_field_def(a_t, "a_pri_field", int_t, ACC_PRIVATE | ACC_STATIC),
  };
  auto a_pub_fun = DexMethod::make_method("LA;", "a_pub_fun", "V", {})
                       ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto a_pro_fun = DexMethod::make_method("LA;", "a_pro_fun", "V", {})
                       ->make_concrete(ACC_PROTECTED | ACC_STATIC, false);

  auto a_pri_fun = DexMethod::make_method("LA;", "a_pri_fun", "V", {})
                       ->make_concrete(ACC_PRIVATE | ACC_STATIC, false);
  std::vector<DexMethod*> a_methods{a_pub_fun, a_pro_fun, a_pri_fun};
  return create_class(
      a_t, type::java_lang_Object(), a_methods, a_fields, ACC_PUBLIC);
}

DexClass* create_class_B(DexType* super) {
  auto b_t = DexType::make_type("LB;");
  std::vector<DexMethod*> b_methods = call_a_fields_and_methods_methods();
  std::vector<DexField*> b_fields{};
  return create_class(b_t, super, b_methods, b_fields);
}

std::vector<DexClass*> create_classes() {
  std::vector<DexClass*> classes;
  // Create A
  auto a_t = DexType::make_type("LA;");
  auto cls_a = create_class_A();
  classes.push_back(cls_a);
  // Create B
  auto cls_b = create_class_B(a_t);
  classes.push_back(cls_b);
  return classes;
}

class CheckBreadcrumbsTest : public RedexTest {};

//========== Test Cases ==========
TEST_F(CheckBreadcrumbsTest, AccessValidityTest) {
  std::vector<DexClass*> classes = create_classes();
  DexMetadata dm;
  dm.set_id("classes");
  DexStore store(dm);
  store.add_classes(classes);
  std::vector<DexStore> stores;
  stores.emplace_back(std::move(store));
  auto scope = build_class_scope(stores);
  Breadcrumbs bc(scope,
                 "",
                 stores,
                 /* reject_illegal_refs_root_store= */ false,
                 /* only_verify_primary_dex= */ false,
                 /* verify_type_hierarchies= */ false,
                 /* verify_proto_cross_dex= */ false,
                 /* enforce_allowed_violations_file= */ false);
  std::vector<DexMethod*> method_list = call_a_fields_and_methods_methods();
  EXPECT_EQ(bc.has_illegal_access(method_list[0]), false);
  EXPECT_EQ(bc.has_illegal_access(method_list[1]), false);
  EXPECT_EQ(bc.has_illegal_access(method_list[2]), true);
  EXPECT_EQ(bc.has_illegal_access(method_list[3]), false);
  EXPECT_EQ(bc.has_illegal_access(method_list[4]), false);
  EXPECT_EQ(bc.has_illegal_access(method_list[5]), true);
  std::ostringstream expected;
  expected << "Bad methods in class LB;\n"
           << "\ta_pri_fun\n\n"
           << "Bad field refs in method LB;.call_a_pri_field\n"
           << "\ta_pri_field\n\n";
  EXPECT_EQ(expected.str(), bc.get_methods_with_bad_refs());
}

} // namespace
