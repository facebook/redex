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
#include "Resolver.h"
#include "Creators.h"

/**
 * Create the following hierarchy
 * external class A
 *   int f1
 * class B extends A
 *   static String f2
 * class C extends B
 *
 * class D
 *   A f
 *
 * class E extends U
 */

DexFieldRef* make_field_ref(DexType* cls, const char* name, DexType* type) {
  return DexField::make_field(cls, DexString::make_string(name), type);
}

DexField* make_field_def(DexType* cls, const char* name, DexType* type,
    DexAccessFlags access = ACC_PUBLIC, bool external = false) {
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

DexClass* create_class(DexType* type, DexType* super,
    std::vector<DexType*>& intfs, std::vector<DexField*>& fields,
    DexAccessFlags access = ACC_PUBLIC, bool external = false) {
  ClassCreator creator(type);
  creator.set_access(access);
  if (external) creator.set_external();
  if (super != nullptr) {
    creator.set_super(super);
  }
  for (const auto intf : intfs) {
    creator.add_interface(intf);
  }
  for (const auto& field : fields) {
    creator.add_field(field);
  }
  return creator.create();
}

DexClass* create_class(DexType* type, DexType* super,
    std::vector<DexField*>& fields,
    DexAccessFlags access = ACC_PUBLIC, bool external = false) {
  std::vector<DexType*> intfs{};
  return create_class(type, super, intfs, fields, access, external);
}

void create_scope() {
  auto obj_t = DexType::make_type("Ljava/lang/Object;");
  auto int_t = DexType::make_type("I");
  auto string_t = DexType::make_type("Ljava/lang/String;");
  auto intf = DexType::make_type("Intf");
  auto a = DexType::make_type("A");
  auto b = DexType::make_type("B");
  auto c = DexType::make_type("C");
  auto d = DexType::make_type("D");
  auto u = DexType::make_type("U");
  auto e = DexType::make_type("E");
  std::vector<DexField*> intf_fields{
      make_field_def(intf, "fin_f", int_t, ACC_PUBLIC | ACC_STATIC | ACC_FINAL, true)};
  auto class_Intf = create_class(intf, obj_t, intf_fields, ACC_PUBLIC | ACC_INTERFACE);
  std::vector<DexField*> a_fields{make_field_def(a, "f1", int_t, ACC_PUBLIC, true)};
  auto cls_A = create_class(a, obj_t, a_fields, ACC_PUBLIC, true);
  std::vector<DexField*> b_fields{make_field_def(b, "f2", string_t, ACC_PUBLIC | ACC_STATIC)};
  std::vector<DexType*> intfs{intf};
  auto cls_B = create_class(b, a, intfs, b_fields);
  std::vector<DexField*> no_fields{};
  auto cls_C = create_class(c, b, no_fields);
  std::vector<DexField*> d_fields{make_field_def(d, "f", a)};
  auto cls_D = create_class(d, obj_t, d_fields);
  auto cls_E = create_class(e, obj_t, no_fields);
}

TEST(ResolveField, empty) {
  g_redex = new RedexContext();
  create_scope();

  // different cases for int A.f1
  DexFieldRef* fdef = DexField::get_field(DexType::get_type("A"),
      DexString::get_string("f1"), DexType::get_type("I"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  DexFieldRef* fref = make_field_ref(
      DexType::get_type("A"), "f1", DexType::get_type("I"));
  EXPECT_TRUE(fdef->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(DexType::get_type("A"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("D"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("B"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(DexType::get_type("B"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("C"),
      DexString::get_string("f1"),
      DexType::get_type("I")) == fdef);
  EXPECT_TRUE(resolve_field(DexType::get_type("C"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Static) == nullptr);
  fref = make_field_ref(
      DexType::get_type("B"), "f1", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);
  fref = make_field_ref(
      DexType::get_type("C"), "f1", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);

  // different cases for static String B.f2
  fdef = DexField::get_field(DexType::get_type("B"),
      DexString::get_string("f2"), DexType::get_type("Ljava/lang/String;"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  fref = make_field_ref(
      DexType::get_type("A"), "f2", DexType::get_type("Ljava/lang/String;"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == nullptr);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);
  fref = make_field_ref(
      DexType::get_type("B"), "f2", DexType::get_type("Ljava/lang/String;"));
  EXPECT_TRUE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  fref = make_field_ref(
      DexType::get_type("C"), "f2", DexType::get_type("Ljava/lang/String;"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);

  // different cases for D.f
  fdef = DexField::get_field(DexType::get_type("D"),
      DexString::get_string("f"), DexType::get_type("A"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  EXPECT_TRUE(resolve_field(fdef) == fdef);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Instance) == fdef);

  // interface final field
  fdef = DexField::get_field(DexType::get_type("Intf"),
      DexString::get_string("fin_f"), DexType::get_type("I"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  EXPECT_TRUE(resolve_field(fdef) == fdef);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Static) == fdef);
  fref = make_field_ref(DexType::get_type("B"), "fin_f", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);
  fref = make_field_ref(DexType::get_type("C"), "fin_f", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);

  // random non existent field
  fdef = DexField::make_field(DexType::get_type("U"),
      DexString::get_string("f"), DexType::get_type("I"));
  EXPECT_FALSE(fdef->is_def());
  EXPECT_TRUE(resolve_field(fdef) == nullptr);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Instance) == nullptr);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("E"),
      DexString::get_string("f1"),
      DexType::get_type("I"),
      FieldSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("E"),
      DexString::get_string("f1"),
      DexType::get_type("Ljava/lang/String;"),
      FieldSearch::Instance) == nullptr);
  EXPECT_TRUE(resolve_field(DexType::get_type("E"),
      DexString::get_string("f1"),
      DexType::get_type("I")) == nullptr);

  delete g_redex;
}
