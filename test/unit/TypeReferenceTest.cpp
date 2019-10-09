/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "TypeReference.h"

#include "Creators.h"
#include "DexClass.h"
#include "RedexTest.h"

using namespace type_reference;

struct TypeReferenceTest : public RedexTest {
  DexClass* m_class;
  std::unordered_map<const DexType*, DexType*> m_old_to_new;
  std::vector<DexClass*> m_scope;

  TypeReferenceTest() {
    auto type = DexType::make_type("Lcom/TestClass;");
    ClassCreator creator(type);
    creator.set_super(get_object_type());
    m_class = creator.create();
    m_scope.push_back(m_class);

    // E; => I
    m_old_to_new.emplace(get_enum_type(), get_int_type());
    // C => Object;
    m_old_to_new.emplace(get_char_type(), get_object_type());
  }

  ~TypeReferenceTest() {}

  DexField* make_a_field(const std::string& name, const DexType* type) {
    auto fname = DexString::make_string(name);
    auto field = static_cast<DexField*>(
        DexField::make_field(m_class->get_type(), fname, type));
    field->make_concrete(ACC_PUBLIC);
    m_class->add_field(field);
    return field;
  }

  void check_proto_update(const DexProto* proto,
                          DexType* rtype,
                          DexTypeList* args) {
    auto new_proto = DexProto::make_proto(rtype, args);
    EXPECT_EQ(new_proto, new_proto);
  }
};

TEST_F(TypeReferenceTest, get_new_proto) {
  auto empty_list = DexTypeList::make_type_list({});

  // ()V => ()V
  auto proto = DexProto::make_proto(get_void_type(), empty_list);
  check_proto_update(proto, get_void_type(), empty_list);

  // ()E; => ()I
  proto = DexProto::make_proto(get_enum_type(), empty_list);
  check_proto_update(proto, get_int_type(), empty_list);

  // (CI)V => (Object;I)V
  proto = DexProto::make_proto(
      get_void_type(),
      DexTypeList::make_type_list({get_char_type(), get_int_type()}));
  check_proto_update(
      proto,
      get_void_type(),
      DexTypeList::make_type_list({get_object_type(), get_int_type()}));

  // ()[C => ()[Object;
  proto = DexProto::make_proto(make_array_type(get_char_type()), empty_list);
  check_proto_update(proto, get_object_type(), empty_list);

  // ()[[E; => ()[[I
  proto = DexProto::make_proto(
      make_array_type(make_array_type(get_enum_type())), empty_list);
  check_proto_update(proto, make_array_type(make_array_type(get_int_type())),
                     empty_list);
}

TEST_F(TypeReferenceTest, update_field_type_references) {
  auto f_b = make_a_field("f_b", get_byte_type());
  auto f_i = make_a_field("f_i", get_int_type());
  auto f_e0 = make_a_field("f_e0", get_enum_type());
  auto f_e1 = make_a_field("f_e1", make_array_type(get_enum_type()));
  auto f_e3 = make_a_field("f_e3", make_array_type(make_array_type(
                                       make_array_type(get_enum_type()))));
  update_field_type_references(m_scope, m_old_to_new);
  // f:B => f:B
  EXPECT_EQ(f_b->get_type(), get_byte_type());
  // f:I => f:I
  EXPECT_EQ(f_i->get_type(), get_int_type());
  // f:E; => f:I
  EXPECT_EQ(f_e0->get_type(), get_int_type());
  // f:[E; => f:[I
  EXPECT_EQ(f_e1->get_type(), make_array_type(get_int_type()));
  // f:[[[E; => f:[[[I
  EXPECT_EQ(f_e3->get_type(),
            make_array_type(make_array_type(make_array_type(get_int_type()))));
}
