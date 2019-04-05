/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/any.hpp>
#include <gtest/gtest.h>

#include <unordered_map>

#include "Creators.h"
#include "DexAnnotation.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "FinalInline.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "Resolver.h"

// Map of type string -> (sget opcode, sput opcode)
static std::unordered_map<std::string, std::pair<IROpcode, IROpcode>> init_ops =
    {
        {"I", {OPCODE_SGET, OPCODE_SPUT}},
        {"Z", {OPCODE_SGET_BOOLEAN, OPCODE_SPUT_BOOLEAN}},
        {"B", {OPCODE_SGET_BYTE, OPCODE_SPUT_BYTE}},
        {"C", {OPCODE_SGET_CHAR, OPCODE_SPUT_CHAR}},
        {"S", {OPCODE_SGET_SHORT, OPCODE_SPUT_SHORT}},
        {"J", {OPCODE_SGET_WIDE, OPCODE_SPUT_WIDE}},
        {"D", {OPCODE_SGET_WIDE, OPCODE_SPUT_WIDE}},
        {"Ljava/lang/String;", {OPCODE_SGET_OBJECT, OPCODE_SPUT_OBJECT}},
};

struct ConstPropTest : testing::Test {
  DexType* m_int_type;
  DexType* m_bool_type;
  DexType* m_byte_type;
  DexType* m_char_type;
  DexType* m_short_type;
  DexType* m_long_type;
  DexType* m_double_type;
  DexType* m_string_type;

  ConstPropTest() {
    g_redex = new RedexContext();
    m_int_type = DexType::make_type("I");
    m_bool_type = DexType::make_type("Z");
    m_byte_type = DexType::make_type("B");
    m_char_type = DexType::make_type("C");
    m_short_type = DexType::make_type("S");
    m_long_type = DexType::make_type("J");
    m_double_type = DexType::make_type("D");
    m_string_type = DexType::make_type("Ljava/lang/String;");
  }

  ~ConstPropTest() { delete g_redex; }

  void expect_empty_clinit(DexClass* clazz) {
    auto clinit = clazz->get_clinit();
    ASSERT_NE(clinit, nullptr) << "Class " << clazz->c_str()
                               << " missing clinit";
    auto code = clinit->get_code();
    EXPECT_EQ(code->count_opcodes(), 0) << "Class " << clazz->c_str()
                                        << " has non-empty clinit";
  }

  void expect_field_eq(DexClass* clazz,
                       const std::string& name,
                       DexType* type,
                       boost::any expected) {
    auto field_name = DexString::make_string(name);
    auto field =
        resolve_field(clazz->get_type(), field_name, type, FieldSearch::Static);
    ASSERT_NE(field, nullptr) << "Failed resolving field " << name
                              << " in class " << clazz->c_str();
    auto val = field->get_static_value();
    ASSERT_NE(val, nullptr) << "Failed getting static value for field "
                            << field->c_str() << " in class " << clazz->c_str();
    if (expected.type() == typeid(uint64_t)) {
      ASSERT_EQ(val->value(), boost::any_cast<uint64_t>(expected))
          << "Incorrect value for field " << field->c_str() << " in class "
          << clazz->c_str();
    } else {
      always_assert(expected.type() == typeid(DexString*));
      ASSERT_EQ(static_cast<DexEncodedValueString*>(val)->string(),
                boost::any_cast<DexString*>(expected))
          << "Incorrect value for field " << field->c_str() << "(\""
          << show(field->get_static_value()) << "\") in class "
          << clazz->c_str();
    }
  }
};

DexEncodedValue* make_ev(DexType* type, boost::any val) {
  if (val.type() == typeid(uint64_t)) {
    auto ev = DexEncodedValue::zero_for_type(type);
    ev->value(boost::any_cast<uint64_t>(val));
    return ev;
  } else {
    // [&]() {
    //   ASSERT_EQ(val.type() == typeid(DexString*), true) << val.type().name();
    // }();
    always_assert(val.type() == typeid(DexString*));
    return new DexEncodedValueString(boost::any_cast<DexString*>(val));
  }
}

// Create the named class with an empty clinit
DexClass* create_class(const std::string& name) {
  auto type = DexType::make_type(DexString::make_string(name));
  ClassCreator creator(type);
  creator.set_super(get_object_type());
  auto cls = creator.create();
  auto clinit_name = DexString::make_string("<clinit>");
  auto void_args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(get_void_type(), void_args);
  auto clinit = static_cast<DexMethod*>(
      DexMethod::make_method(type, clinit_name, void_void));
  clinit->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
  clinit->set_code(std::make_unique<IRCode>(clinit, 1));
  cls->add_method(clinit);
  return cls;
}

// Add a field that is initialized to a value
DexField* add_concrete_field(DexClass* cls,
                             const std::string& name,
                             DexType* type,
                             boost::any val) {
  auto container = cls->get_type();
  auto field_name = DexString::make_string(name);
  auto field = static_cast<DexField*>(
      DexField::make_field(container, field_name, type));
  auto ev = make_ev(type, val);
  field->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL, ev);
  cls->add_field(field);
  return field;
}

// Add a field that is initialized to the value of parent
DexField* add_dependent_field(DexClass* cls,
                              const std::string& name,
                              DexField* parent) {
  // Create the field
  auto container = cls->get_type();
  auto field_name = DexString::make_string(name);
  auto field = static_cast<DexField*>(
      DexField::make_field(container, field_name, parent->get_type()));
  field->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
  cls->add_field(field);
  // Initialize it to the value of the parent
  auto parent_type = parent->get_type();
  redex_assert(init_ops.count(parent_type->c_str()) != 0);
  auto ops = init_ops[parent_type->c_str()];
  auto clinit = cls->get_clinit();
  auto code = clinit->get_code();
  auto sget = new IRInstruction(ops.first);
  sget->set_field(parent);
  code->push_back(sget);
  code->push_back(
      (new IRInstruction(opcode::move_result_pseudo_for_sget(ops.first)))
          ->set_dest(0));
  auto sput = new IRInstruction(ops.second);
  sput->set_field(field)->set_src(0, 0);
  code->push_back(sput);
  return field;
}

struct FieldDescriptor {
  std::string name;
  DexType* type;
  boost::any value;
};

// Check that we can do a simple, single level propagation. As source, this
// would look like:
//
//   class Parent {
//     public static final int CONST = 1;
//   }
//
//   class Child {
//     public static final int CONST = Parent.CONST;
//   }
TEST_F(ConstPropTest, simplePropagate) {
  FieldDescriptor test_cases[] = {
      {"int", m_int_type, static_cast<uint64_t>(12345)},
      {"bool", m_bool_type, static_cast<uint64_t>(1)},
      {"byte", m_byte_type, static_cast<uint64_t>('b')},
      {"char", m_char_type, static_cast<uint64_t>('c')},
      {"short", m_short_type, static_cast<uint64_t>(256)}};
  for (auto test_case : test_cases) {
    auto parent = create_class("Lcom/redex/Parent_" + test_case.name + ";");
    auto parent_field =
        add_concrete_field(parent, "CONST", test_case.type, test_case.value);

    auto child = create_class("Lcom/redex/Child_" + test_case.name + ";");
    auto child_field = add_dependent_field(child, "CONST", parent_field);

    Scope classes = {parent, child};
    FinalInlinePass::propagate_constants_for_test(
        classes, /*string*/ true, /*wide*/ true);

    expect_empty_clinit(child);
    expect_field_eq(child, "CONST", test_case.type, test_case.value);
  }
}

// Check that we can do a simple, single level propagation with multiple fields.
// As source, this would look like:
//
//   class Parent {
//     public static final int CONST_INT = 1111;
//     public static final bool CONST_BOOL = false;
//     public static final byte CONST_BYTE = 'b';
//     public static final char CONST_CHAR = 'c';
//     public static final short CONST_SHORT = 555;
//     public static final short CONST_LONG = 0x1000200030004000;
//     public static final short CONST_DOUBLE = 1.0000000000000002;
//     public static final String CONST_STRING = "foo";
//   }
//
//   class Child {
//     public static final int CONST_INT = Parent.CONST_INT;
//     public static final bool CONST_BOOL = Parent.CONST_BOOL;
//     public static final byte CONST_BYTE = Parent.CONST_BYTE;
//     public static final char CONST_CHAR = Parent.CONST_CHAR;
//     public static final short CONST_SHORT = Parent.CONST_SHORT;
//     public static final short CONST_LONG = Parent.CONST_LONG;
//     public static final short CONST_DOUBLE = Parent.CONST_DOUBLE;
//     public static final String CONST_STRING = Parent.CONST_STRING;
//   }
TEST_F(ConstPropTest, simplePropagateMultiField) {
  FieldDescriptor field_descs[] = {
      {"CONST_INT", m_int_type, static_cast<uint64_t>(1111)},
      {"CONST_BOOL", m_bool_type, static_cast<uint64_t>(0)},
      {"CONST_BYTE", m_byte_type, static_cast<uint64_t>('b')},
      {"CONST_CHAR", m_char_type, static_cast<uint64_t>('c')},
      {"CONST_SHORT", m_short_type, static_cast<uint64_t>(555)},
      {"CONST_LONG", m_long_type, static_cast<uint64_t>(0x1000200030004000)},
      {"CONST_DOUBLE", m_double_type, static_cast<uint64_t>(1.0000000000000002)},
      {"CONST_STRING", m_string_type, DexString::make_string("foo")}};
  auto parent = create_class("Lcom/redex/Parent;");
  auto child = create_class("Lcom/redex/Child;");
  for (auto fd : field_descs) {
    auto parent_field = add_concrete_field(parent, fd.name, fd.type, fd.value);
    add_dependent_field(child, fd.name, parent_field);
  }
  Scope classes = {parent, child};
  FinalInlinePass::propagate_constants_for_test(
      classes, /*string*/ true, /*wide*/ true);

  expect_empty_clinit(child);
  for (auto fd : field_descs) {
    expect_field_eq(child, fd.name, fd.type, fd.value);
  }
}

// Check that we can do a simple, single level propagation with multiple fields.
// As source, this would look like:
//
//   class Parent {
//     public static final int CONST_INT = 1111;
//     public static final bool CONST_BOOL = false;
//     public static final byte CONST_BYTE = 'b';
//     public static final char CONST_CHAR = 'c';
//     public static final short CONST_SHORT = 555;
//     public static final String CONST_STRING = "foo";
//   }
//
//   class Child {
//     public static final int CONST_INT = Parent.CONST_INT;
//     public static final bool CONST_BOOL = Parent.CONST_BOOL;
//     public static final byte CONST_BYTE = Parent.CONST_BYTE;
//     public static final char CONST_CHAR = Parent.CONST_CHAR;
//     public static final short CONST_SHORT = Parent.CONST_SHORT;
//     public static final String CONST_STRING = Parent.CONST_STRING;
//   }
TEST_F(ConstPropTest, simplePropagateMultiFieldNoWide) {
  FieldDescriptor field_descs[] = {
      {"CONST_INT", m_int_type, static_cast<uint64_t>(1111)},
      {"CONST_BOOL", m_bool_type, static_cast<uint64_t>(0)},
      {"CONST_BYTE", m_byte_type, static_cast<uint64_t>('b')},
      {"CONST_CHAR", m_char_type, static_cast<uint64_t>('c')},
      {"CONST_SHORT", m_short_type, static_cast<uint64_t>(555)},
      {"CONST_STRING", m_string_type, DexString::make_string("foo")}};
  auto parent = create_class("Lcom/redex/Parent;");
  auto child = create_class("Lcom/redex/Child;");
  for (auto fd : field_descs) {
    auto parent_field = add_concrete_field(parent, fd.name, fd.type, fd.value);
    add_dependent_field(child, fd.name, parent_field);
  }
  Scope classes = {parent, child};
  FinalInlinePass::propagate_constants_for_test(
      classes, /*string*/ true, /*wide*/ false);

  expect_empty_clinit(child);
  for (auto fd : field_descs) {
    expect_field_eq(child, fd.name, fd.type, fd.value);
  }
}

// Check that we can propagate across multiple levels of dependencies. As
// source, this looks like:
//   class Parent {
//     public static final int CONST_INT = 1111;
//     public static final bool CONST_BOOL = false;
//     public static final byte CONST_BYTE = 'b';
//     public static final char CONST_CHAR = 'c';
//     public static final short CONST_SHORT = 555;
//     public static final short CONST_LONG = 0x1000200030004000;
//     public static final short CONST_DOUBLE = 1.0000000000000002;
//     public static final String CONST_STRING = "foo";
//   }
//
//   class Child {
//     public static final int CONST_INT = Parent.CONST_INT;
//     public static final bool CONST_BOOL = Parent.CONST_BOOL;
//     public static final byte CONST_BYTE = Parent.CONST_BYTE;
//     public static final char CONST_CHAR = Parent.CONST_CHAR;
//     public static final short CONST_SHORT = Parent.CONST_SHORT;
//     public static final short CONST_LONG = Parent.CONST_LONG;
//     public static final short CONST_DOUBLE = Parent.CONST_DOUBLE;
//     public static final String CONST_STRING = Parent.CONST_STRING;
//   }
//
//   class GrandChild {
//     public static final int CONST_INT = Child.CONST_INT;
//     public static final bool CONST_BOOL = Child.CONST_BOOL;
//     public static final byte CONST_BYTE = Child.CONST_BYTE;
//     public static final char CONST_CHAR = Child.CONST_CHAR;
//     public static final short CONST_SHORT = Child.CONST_SHORT;
//     public static final short CONST_LONG = Child.CONST_LONG;
//     public static final short CONST_DOUBLE = Child.CONST_DOUBLE;
//     public static final String CONST_STRING = Child.CONST_STRING;
//   }
TEST_F(ConstPropTest, multiLevelPropagate) {
  FieldDescriptor field_descs[] = {
      {"CONST_INT", m_int_type, static_cast<uint64_t>(1111)},
      {"CONST_BOOL", m_bool_type, static_cast<uint64_t>(0)},
      {"CONST_BYTE", m_byte_type, static_cast<uint64_t>('b')},
      {"CONST_CHAR", m_char_type, static_cast<uint64_t>('c')},
      {"CONST_SHORT", m_short_type, static_cast<uint64_t>(555)},
      {"CONST_LONG", m_long_type, static_cast<uint64_t>(0x1000200030004000)},
      {"CONST_DOUBLE", m_double_type, static_cast<uint64_t>(1.0000000000000002)},
      {"CONST_STRING", m_string_type, DexString::make_string("foo")}};
  auto parent = create_class("Lcom/redex/Parent;");
  auto child = create_class("Lcom/redex/Child;");
  auto grandchild = create_class("Lcom/redex/GrandChild;");
  for (auto fd : field_descs) {
    auto parent_field = add_concrete_field(parent, fd.name, fd.type, fd.value);
    auto child_field = add_dependent_field(child, fd.name, parent_field);
    add_dependent_field(grandchild, fd.name, child_field);
  }

  Scope classes = {parent, child, grandchild};
  FinalInlinePass::propagate_constants_for_test(
      classes, /*string*/ true, /*wide*/ true);

  std::vector<DexClass*> descendants = {child, grandchild};
  for (auto clazz : descendants) {
    expect_empty_clinit(clazz);
    for (auto fd : field_descs) {
      expect_field_eq(clazz, fd.name, fd.type, fd.value);
    }
  }
}

// Check that we can propagate across multiple levels of dependencies where
// there are siblings at each level. In source, this looks like:
//
//   class Parent1 {
//     public static final int CONST_INT = 1111;
//     public static final char CONST_CHAR = 'a';
//     public static final String CONST_STRING = "foo";
//   }
//
//   class Parent2 {
//     public static final int CONST_INT = 2222;
//     public static final char CONST_CHAR = 'b';
//     public static final String CONST_STRING = "bar";
//   }
//
//   class Child1 {
//     public static final int CONST_INT = Parent1.CONST_INT;
//     public static final char CONST_CHAR = Parent2.CONST_CHAR;
//     public static final String CONST_STRING = Parent1.CONST_STRING;
//     public static final bool CONST_BOOL = true;
//   }
//
//   class Child2 {
//     public static final int CONST_INT = Parent2.CONST_INT;
//     public static final char CONST_CHAR = Parent1.CONST_CHAR;
//     public static final String CONST_STRING = Parent2.CONST_STRING;
//     public static final bool CONST_BOOL = false;
//   }
//
//   class GrandChild1 {
//     public static final int CONST_INT = Child1.CONST_INT;
//     public static final char CONST_CHAR = Child1.CONST_CHAR;
//     public static final bool CONST_BOOL = Child1.CONST_BOOL;
//     public static final String CONST_STRING = Child1.CONST_STRING;
//   }
//
//   class GrandChild2 {
//     public static final int CONST_INT = Child2.CONST_INT;
//     public static final int CONST_CHAR = Child2.CONST_CHAR;
//     public static final bool CONST_BOOL = Child2.CONST_BOOL;
//     public static final String CONST_STRING = Child2.CONST_STRING;
//   }
TEST_F(ConstPropTest, multiLevelWithSiblings) {
  auto parent1 = create_class("Lcom/redex/Parent1;");
  auto parent1_int = add_concrete_field(
      parent1, "CONST_INT", m_int_type, static_cast<uint64_t>(1111));
  auto parent1_char = add_concrete_field(
      parent1, "CONST_CHAR", m_char_type, static_cast<uint64_t>('a'));
  auto parent1_string = add_concrete_field(
      parent1, "CONST_STRING", m_char_type, DexString::make_string("foo"));

  auto parent2 = create_class("Lcom/redex/Parent2;");
  auto parent2_int = add_concrete_field(
      parent2, "CONST_INT", m_int_type, static_cast<uint64_t>(2222));
  auto parent2_char = add_concrete_field(
      parent2, "CONST_CHAR", m_char_type, static_cast<uint64_t>('b'));
  auto parent2_string = add_concrete_field(
      parent2, "CONST_STRING", m_char_type, DexString::make_string("bar"));

  auto child1 = create_class("Lcom/redex/Child1;");
  auto child1_int = add_dependent_field(child1, "CONST_INT", parent1_int);
  auto child1_char = add_dependent_field(child1, "CONST_CHAR", parent2_char);
  auto child1_string =
      add_dependent_field(child1, "CONST_STRING", parent1_string);
  auto child1_bool = add_concrete_field(
      child1, "CONST_BOOL", m_bool_type, static_cast<uint64_t>(1));

  auto child2 = create_class("Lcom/redex/Child2;");
  auto child2_int = add_dependent_field(child2, "CONST_INT", parent2_int);
  auto child2_char = add_dependent_field(child2, "CONST_CHAR", parent1_char);
  auto child2_string =
      add_dependent_field(child2, "CONST_STRING", parent2_string);
  auto child2_bool = add_concrete_field(
      child2, "CONST_BOOL", m_bool_type, static_cast<uint64_t>(0));

  auto grandchild1 = create_class("Lcom/redex/GrandChild1;");
  add_dependent_field(grandchild1, "CONST_INT", child1_int);
  add_dependent_field(grandchild1, "CONST_CHAR", child1_char);
  add_dependent_field(grandchild1, "CONST_BOOL", child1_bool);
  add_dependent_field(grandchild1, "CONST_STRING", child1_string);

  auto grandchild2 = create_class("Lcom/redex/GrandChild2;");
  add_dependent_field(grandchild2, "CONST_INT", child2_int);
  add_dependent_field(grandchild2, "CONST_CHAR", child2_char);
  add_dependent_field(grandchild2, "CONST_BOOL", child2_bool);
  add_dependent_field(grandchild2, "CONST_STRING", child2_string);

  Scope classes = {parent1, parent2, child1, child2, grandchild1, grandchild2};
  FinalInlinePass::propagate_constants_for_test(
      classes, /*string*/ true, /*wide*/ true);

  Scope descendents = {child1, child2, grandchild1, grandchild2};
  for (auto clazz : descendents) {
    expect_empty_clinit(clazz);
  }

  expect_field_eq(child1, "CONST_INT", m_int_type, static_cast<uint64_t>(1111));
  expect_field_eq(
      child1, "CONST_CHAR", m_char_type, static_cast<uint64_t>('b'));
  expect_field_eq(child2, "CONST_INT", m_int_type, static_cast<uint64_t>(2222));
  expect_field_eq(
      child2, "CONST_CHAR", m_char_type, static_cast<uint64_t>('a'));
  expect_field_eq(
      grandchild1, "CONST_INT", m_int_type, static_cast<uint64_t>(1111));
  expect_field_eq(
      grandchild1, "CONST_CHAR", m_char_type, static_cast<uint64_t>('b'));
  expect_field_eq(
      grandchild1, "CONST_BOOL", m_bool_type, static_cast<uint64_t>(1));
  expect_field_eq(
      grandchild2, "CONST_INT", m_int_type, static_cast<uint64_t>(2222));
  expect_field_eq(
      grandchild2, "CONST_CHAR", m_char_type, static_cast<uint64_t>('a'));
  expect_field_eq(
      grandchild2, "CONST_BOOL", m_bool_type, static_cast<uint64_t>(0));
}
