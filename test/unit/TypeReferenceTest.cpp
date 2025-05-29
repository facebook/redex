/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  UnorderedMap<const DexType*, DexType*> m_old_to_new;
  std::vector<DexClass*> m_scope;

  TypeReferenceTest() {
    auto type = DexType::make_type("Lcom/TestClass;");
    ClassCreator creator(type);
    creator.set_super(type::java_lang_Object());
    m_class = creator.create();
    m_scope.push_back(m_class);

    // E; => I
    m_old_to_new.emplace(type::java_lang_Enum(), type::_int());
    // C => Object;
    m_old_to_new.emplace(type::_char(), type::java_lang_Object());
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
  auto proto = DexProto::make_proto(type::_void(), empty_list);
  check_proto_update(proto, type::_void(), empty_list);

  // ()E; => ()I
  proto = DexProto::make_proto(type::java_lang_Enum(), empty_list);
  check_proto_update(proto, type::_int(), empty_list);

  // (CI)V => (Object;I)V
  proto = DexProto::make_proto(
      type::_void(),
      DexTypeList::make_type_list({type::_char(), type::_int()}));
  check_proto_update(
      proto,
      type::_void(),
      DexTypeList::make_type_list({type::java_lang_Object(), type::_int()}));

  // ()[C => ()[Object;
  proto =
      DexProto::make_proto(type::make_array_type(type::_char()), empty_list);
  check_proto_update(proto, type::java_lang_Object(), empty_list);

  // ()[[E; => ()[[I
  proto = DexProto::make_proto(
      type::make_array_type(type::make_array_type(type::java_lang_Enum())),
      empty_list);
  check_proto_update(proto,
                     type::make_array_type(type::make_array_type(type::_int())),
                     empty_list);
}

TEST_F(TypeReferenceTest, update_field_type_references) {
  auto f_b = make_a_field("f_b", type::_byte());
  auto f_i = make_a_field("f_i", type::_int());
  auto f_e0 = make_a_field("f_e0", type::java_lang_Enum());
  auto f_e1 =
      make_a_field("f_e1", type::make_array_type(type::java_lang_Enum()));
  auto f_e3 = make_a_field("f_e3",
                           type::make_array_type(type::make_array_type(
                               type::make_array_type(type::java_lang_Enum()))));
  update_field_type_references(m_scope, m_old_to_new);
  // f:B => f:B
  EXPECT_EQ(f_b->get_type(), type::_byte());
  // f:I => f:I
  EXPECT_EQ(f_i->get_type(), type::_int());
  // f:E; => f:I
  EXPECT_EQ(f_e0->get_type(), type::_int());
  // f:[E; => f:[I
  EXPECT_EQ(f_e1->get_type(), type::make_array_type(type::_int()));
  // f:[[[E; => f:[[[I
  EXPECT_EQ(f_e3->get_type(),
            type::make_array_type(
                type::make_array_type(type::make_array_type(type::_int()))));
}
