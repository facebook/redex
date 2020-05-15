/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <memory>

#include "Creators.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "Resolver.h"

DexFieldRef* make_field_ref(DexType* cls, const char* name, DexType* type) {
  return DexField::make_field(cls, DexString::make_string(name), type);
}

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

DexClass* create_class(DexType* type,
                       DexType* super,
                       std::vector<DexType*>& intfs,
                       std::vector<DexField*>& fields,
                       DexAccessFlags access = ACC_PUBLIC,
                       bool external = false) {
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

DexClass* create_class(DexType* type,
                       DexType* super,
                       std::vector<DexField*>& fields,
                       DexAccessFlags access = ACC_PUBLIC,
                       bool external = false) {
  std::vector<DexType*> intfs{};
  return create_class(type, super, intfs, fields, access, external);
}

DexMethodRef* create_method(DexClass* cls,
                            const char* method_name,
                            DexAccessFlags access = ACC_PUBLIC,
                            bool concrete = true,
                            bool is_virtual = true) {
  DexProto* proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  DexMethodRef* method_ref = DexMethod::make_method(
      cls->get_type(), DexString::make_string(method_name), proto);
  if (concrete) {
    DexMethod* method = method_ref->make_concrete(access, is_virtual);
    cls->add_method(method);
  }
  return method_ref;
}

/**
 * Create the following hierarchy:
 *
 * interface IntF
 *   static final int fin_f
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
void create_field_scope() {
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
  std::vector<DexField*> intf_fields{make_field_def(
      intf, "fin_f", int_t, ACC_PUBLIC | ACC_STATIC | ACC_FINAL, true)};
  auto class_Intf =
      create_class(intf, obj_t, intf_fields, ACC_PUBLIC | ACC_INTERFACE);
  std::vector<DexField*> a_fields{
      make_field_def(a, "f1", int_t, ACC_PUBLIC, true)};
  auto cls_A = create_class(a, obj_t, a_fields, ACC_PUBLIC, true);
  std::vector<DexField*> b_fields{
      make_field_def(b, "f2", string_t, ACC_PUBLIC | ACC_STATIC)};
  std::vector<DexType*> intfs{intf};
  auto cls_B = create_class(b, a, intfs, b_fields);
  std::vector<DexField*> no_fields{};
  auto cls_C = create_class(c, b, no_fields);
  std::vector<DexField*> d_fields{make_field_def(d, "f", a)};
  auto cls_D = create_class(d, obj_t, d_fields);
  auto cls_E = create_class(e, obj_t, no_fields);
}

/**
 * Create the following hierarchy:
 *
 * interface A
 *   void method()
 * class B implements A
 *   void method()
 * class C extends B
 *
 * class D extends C
 *   void method()
 */
void create_method_scope() {
  auto obj_t = DexType::make_type("Ljava/lang/Object;");
  auto a = DexType::make_type("A");
  auto b = DexType::make_type("B");
  auto c = DexType::make_type("C");
  auto d = DexType::make_type("D");
  std::vector<DexField*> no_fields{};
  std::vector<DexType*> interfaces_a{a};
  auto interface_a =
      create_class(a, obj_t, no_fields, ACC_PUBLIC | ACC_INTERFACE);
  create_method(interface_a, "method", ACC_PUBLIC | ACC_INTERFACE);
  auto cls_b = create_class(b, obj_t, interfaces_a, no_fields, ACC_PUBLIC);
  create_method(cls_b, "method", ACC_PUBLIC);
  auto cls_c = create_class(c, b, interfaces_a, no_fields, ACC_PUBLIC);
  create_method(cls_c, "method", ACC_PUBLIC, /* concrete */ false);
  auto cls_d = create_class(d, c, interfaces_a, no_fields, ACC_PUBLIC);
  create_method(cls_d, "method", ACC_PUBLIC);
}

class ResolverTest : public RedexTest {};

TEST_F(ResolverTest, ResolveField) {
  create_field_scope();

  // different cases for int A.f1
  DexFieldRef* fdef =
      DexField::get_field(DexType::get_type("A"), DexString::get_string("f1"),
                          DexType::get_type("I"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  DexFieldRef* fref =
      make_field_ref(DexType::get_type("A"), "f1", DexType::get_type("I"));
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
  fref = make_field_ref(DexType::get_type("B"), "f1", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);
  fref = make_field_ref(DexType::get_type("C"), "f1", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);

  // different cases for static String B.f2
  fdef =
      DexField::get_field(DexType::get_type("B"), DexString::get_string("f2"),
                          DexType::get_type("Ljava/lang/String;"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  fref = make_field_ref(DexType::get_type("A"), "f2",
                        DexType::get_type("Ljava/lang/String;"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == nullptr);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == nullptr);
  fref = make_field_ref(DexType::get_type("B"), "f2",
                        DexType::get_type("Ljava/lang/String;"));
  EXPECT_TRUE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == fdef);
  fref = make_field_ref(DexType::get_type("C"), "f2",
                        DexType::get_type("Ljava/lang/String;"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);

  // different cases for D.f
  fdef = DexField::get_field(DexType::get_type("D"), DexString::get_string("f"),
                             DexType::get_type("A"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  EXPECT_TRUE(resolve_field(fdef) == fdef);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Instance) == fdef);

  // interface final field
  fdef = DexField::get_field(DexType::get_type("Intf"),
                             DexString::get_string("fin_f"),
                             DexType::get_type("I"));
  EXPECT_TRUE(fdef != nullptr && fdef->is_def());
  EXPECT_TRUE(resolve_field(fdef) == fdef);
  EXPECT_TRUE(resolve_field(fdef, FieldSearch::Static) == fdef);
  fref =
      make_field_ref(DexType::get_type("B"), "fin_f", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);
  fref =
      make_field_ref(DexType::get_type("C"), "fin_f", DexType::get_type("I"));
  EXPECT_FALSE(fref->is_def());
  EXPECT_TRUE(resolve_field(fref) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Static) == fdef);
  EXPECT_TRUE(resolve_field(fref, FieldSearch::Instance) == nullptr);

  // random non existent field
  fdef =
      DexField::make_field(DexType::get_type("U"), DexString::get_string("f"),
                           DexType::get_type("I"));
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
}

TEST_F(ResolverTest, ResolveMethod) {
  create_method_scope();

  auto a_method = DexMethod::get_method("A.method:()V");
  EXPECT_TRUE(a_method != nullptr && a_method->is_def());

  auto b_method = DexMethod::get_method("B.method:()V");
  EXPECT_TRUE(b_method != nullptr && b_method->is_def());

  auto c_method = DexMethod::get_method("C.method:()V");
  EXPECT_TRUE(c_method != nullptr && !c_method->is_def());

  auto d_method = DexMethod::get_method("D.method:()V");
  EXPECT_TRUE(d_method != nullptr && d_method->is_def());

  auto a_method_def = a_method->as_def();
  auto b_method_def = b_method->as_def();
  auto d_method_def = d_method->as_def();

  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Direct) == a_method);
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Static) == a_method);
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Virtual) == a_method);
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Any) == a_method);
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Interface) == a_method);

  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Direct) == b_method);
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Static) == b_method);
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Virtual) == b_method);
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Any) == b_method);
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Interface) == b_method);

  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Direct) == nullptr);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Static) == nullptr);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Virtual) == b_method);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Any) == b_method);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Interface) == a_method);

  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Direct) == d_method);
  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Static) == d_method);
  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Virtual) == d_method);
  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Any) == d_method);
  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Interface) == d_method);

  // Super class of A doesn't have such method.
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Super, a_method_def) ==
              nullptr);
  EXPECT_TRUE(resolve_method(type_class(a_method->get_class()),
                             a_method->get_name(), a_method->get_proto(),
                             MethodSearch::Super, a_method_def) == nullptr);
  // A is an interface, B cannot use invoke-super on method def in A.
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Super, b_method_def) ==
              nullptr);
  EXPECT_TRUE(resolve_method(type_class(b_method->get_class()),
                             b_method->get_name(), b_method->get_proto(),
                             MethodSearch::Super, b_method_def) == nullptr);
  // Generally class in the method ref doesn't matter.
  EXPECT_TRUE(resolve_method(a_method, MethodSearch::Super, d_method_def) ==
              b_method);
  EXPECT_TRUE(resolve_method(type_class(a_method->get_class()),
                             a_method->get_name(), a_method->get_proto(),
                             MethodSearch::Super, d_method_def) == b_method);
  EXPECT_TRUE(resolve_method(b_method, MethodSearch::Super, d_method_def) ==
              b_method);
  EXPECT_TRUE(resolve_method(type_class(b_method->get_class()),
                             b_method->get_name(), b_method->get_proto(),
                             MethodSearch::Super, d_method_def) == b_method);
  EXPECT_TRUE(resolve_method(d_method, MethodSearch::Super, d_method_def) ==
              b_method);
  EXPECT_TRUE(resolve_method(type_class(d_method->get_class()),
                             d_method->get_name(), d_method->get_proto(),
                             MethodSearch::Super, d_method_def) == b_method);

  MethodRefCache ref_cache;
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Direct, ref_cache) ==
              nullptr);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Static, ref_cache) ==
              nullptr);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Virtual, ref_cache) ==
              b_method);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Any, ref_cache) ==
              b_method);
  EXPECT_TRUE(resolve_method(c_method, MethodSearch::Interface, ref_cache) ==
              a_method);
}
