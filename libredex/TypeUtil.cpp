/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeUtil.h"

namespace type {

DexType* _void() { return DexType::make_type("V"); }

DexType* _byte() { return DexType::make_type("B"); }

DexType* _char() { return DexType::make_type("C"); }

DexType* _short() { return DexType::make_type("S"); }

DexType* _int() { return DexType::make_type("I"); }

DexType* _long() { return DexType::make_type("J"); }

DexType* _boolean() { return DexType::make_type("Z"); }

DexType* _float() { return DexType::make_type("F"); }

DexType* _double() { return DexType::make_type("D"); }

DexType* java_lang_String() { return DexType::make_type("Ljava/lang/String;"); }

DexType* java_lang_Class() { return DexType::make_type("Ljava/lang/Class;"); }

DexType* java_lang_Enum() { return DexType::make_type("Ljava/lang/Enum;"); }

DexType* java_lang_Object() { return DexType::make_type("Ljava/lang/Object;"); }

DexType* java_lang_Void() { return DexType::make_type("Ljava/lang/Void;"); }

DexType* java_lang_Throwable() {
  return DexType::make_type("Ljava/lang/Throwable;");
}

DexType* java_lang_Boolean() {
  return DexType::make_type("Ljava/lang/Boolean;");
}

DexType* java_lang_Byte() { return DexType::make_type("Ljava/lang/Byte;"); }

DexType* java_lang_Short() { return DexType::make_type("Ljava/lang/Short;"); }

DexType* java_lang_Character() {
  return DexType::make_type("Ljava/lang/Character;");
}

DexType* java_lang_Integer() {
  return DexType::make_type("Ljava/lang/Integer;");
}

DexType* java_lang_Long() { return DexType::make_type("Ljava/lang/Long;"); }

DexType* java_lang_Float() { return DexType::make_type("Ljava/lang/Float;"); }

DexType* java_lang_Double() { return DexType::make_type("Ljava/lang/Double;"); }

bool is_primitive(const DexType* type) {
  auto* const name = type->get_name()->c_str();
  switch (name[0]) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
  case 'V':
    return true;
  case 'L':
  case '[':
    return false;
  }
  not_reached();
}

bool is_wide_type(const DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case 'J':
  case 'D':
    return true;
  default:
    return false;
  }
  not_reached();
}

bool is_array(const DexType* type) {
  return type->get_name()->c_str()[0] == '[';
}

bool is_object(const DexType* type) {
  char sig = type->get_name()->c_str()[0];
  return (sig == 'L') || (sig == '[');
}

bool is_java_lang_object_array(const DexType* type) {
  if (!is_array(type)) {
    return false;
  }
  return get_array_component_type(type) == java_lang_Object();
}

bool is_reference_array(const DexType* type) {
  if (!is_array(type)) {
    return false;
  }
  auto ctype = get_array_component_type(type);
  return !is_primitive(ctype);
}

bool is_integer(const DexType* type) {
  char sig = type->get_name()->c_str()[0];
  switch (sig) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I': {
    return true;
  }
  default: {
    return false;
  }
  }
}

bool is_boolean(const DexType* type) {
  return type->get_name()->c_str()[0] == 'Z';
}

bool is_long(const DexType* type) {
  return type->get_name()->c_str()[0] == 'J';
}

bool is_float(const DexType* type) {
  return type->get_name()->c_str()[0] == 'F';
}

bool is_double(const DexType* type) {
  return type->get_name()->c_str()[0] == 'D';
}

bool is_void(const DexType* type) {
  return type->get_name()->c_str()[0] == 'V';
}

char type_shorty(const DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case '[':
    return 'L';
  case 'V':
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
  case 'L':
    return name[0];
  }
  not_reached();
}

bool check_cast(const DexType* type, const DexType* base_type) {
  if (type == base_type) return true;
  const auto cls = type_class(type);
  if (cls == nullptr) return false;
  if (check_cast(cls->get_super_class(), base_type)) return true;
  auto intfs = cls->get_interfaces();
  for (auto intf : intfs->get_type_list()) {
    if (check_cast(intf, base_type)) return true;
  }
  return false;
}

/**
 * Lcom/facebook/ClassA; ==> Lcom/facebook/
 */
std::string get_package_name(const DexType* type) {
  const auto& name = type->get_name()->str();
  auto pos = name.find_last_of('/');
  if (pos == std::string::npos) {
    return "";
  }
  return name.substr(0, pos + 1);
}

bool same_package(const DexType* type1, const DexType* type2) {
  auto package1 = get_package_name(type1);
  auto package2 = get_package_name(type2);
  auto min_len = std::min(package1.size(), package2.size());
  return package1.compare(0, min_len, package2, 0, min_len) == 0;
}

std::string get_simple_name(const DexType* type) {
  std::string name = std::string(type->get_name()->c_str());
  if (name.find('/') == std::string::npos) {
    return name;
  }
  unsigned long pos_begin = name.find_last_of('/');
  unsigned long pos_end = name.find_last_of(';');
  return name.substr(pos_begin + 1, pos_end - pos_begin - 1);
}

uint32_t get_array_level(const DexType* type) {
  auto name = type->get_name()->c_str();
  uint32_t level = 0;
  while (*name++ == '[' && ++level)
    ;
  return level;
}

DexType* get_array_component_type(const DexType* type) {
  if (!is_array(type)) return nullptr;
  auto name = type->get_name()->c_str();
  name++;
  return DexType::make_type(name);
}

DexType* get_array_element_type(const DexType* type) {
  if (!is_array(type)) {
    return nullptr;
  }
  auto name = type->get_name()->c_str();
  while (*name == '[') {
    name++;
  }
  return DexType::make_type(name);
}

const DexType* get_element_type_if_array(const DexType* type) {
  if (is_array(type)) {
    return get_array_element_type(type);
  }
  return type;
}

DexType* make_array_type(const DexType* type) {
  always_assert(type != nullptr);
  return DexType::make_type(
      DexString::make_string("[" + type->get_name()->str()));
}

DexType* make_array_type(const DexType* type, uint32_t level) {
  always_assert(type != nullptr);
  if (level == 0) {
    return const_cast<DexType*>(type);
  }
  const auto elem_name = type->str();
  const uint32_t size = elem_name.size() + level;
  std::string name;
  name.reserve(size + 1);
  name.append(level, '[');
  name.append(elem_name.begin(), elem_name.end());
  return DexType::make_type(name.c_str(), name.size());
}

DexType* get_boxed_reference_type(const DexType* type) {
  switch (type::type_shorty(type)) {
  case 'Z':
    return type::java_lang_Boolean();
  case 'B':
    return type::java_lang_Byte();
  case 'S':
    return type::java_lang_Short();
  case 'C':
    return type::java_lang_Character();
  case 'I':
    return type::java_lang_Integer();
  case 'J':
    return type::java_lang_Long();
  case 'F':
    return type::java_lang_Float();
  case 'D':
    return type::java_lang_Double();
  default:
    return nullptr;
  }
}

// Takes a reference type, returns its corresponding unboxing method
DexMethodRef* get_unboxing_method_for_type(const DexType* type) {
  if (type == type::java_lang_Boolean()) {
    return DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z");
  } else if (type == type::java_lang_Byte()) {
    return DexMethod::make_method("Ljava/lang/Byte;.byteValue:()B");
  } else if (type == type::java_lang_Short()) {
    return DexMethod::make_method("Ljava/lang/Short;.shortValue:()S");
  } else if (type == type::java_lang_Character()) {
    return DexMethod::make_method("Ljava/lang/Character;.charValue:()C");
  } else if (type == type::java_lang_Integer()) {
    return DexMethod::make_method("Ljava/lang/Integer;.intValue:()I");
  } else if (type == type::java_lang_Long()) {
    return DexMethod::make_method("Ljava/lang/Long;.longValue:()J");
  } else if (type == type::java_lang_Float()) {
    return DexMethod::make_method("Ljava/lang/Float;.floatValue:()F");
  } else if (type == type::java_lang_Double()) {
    return DexMethod::make_method("Ljava/lang/Double;.doubleValue:()D");
  }
  return nullptr;
}

// Take a reference type, returns its valueOf function
DexMethodRef* get_value_of_method_for_type(const DexType* type) {
  if (type == type::java_lang_Boolean()) {
    return DexMethod::make_method(
        "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;");
  } else if (type == type::java_lang_Byte()) {
    return DexMethod::make_method(
        "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;");
  } else if (type == type::java_lang_Short()) {
    return DexMethod::make_method(
        "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;");
  } else if (type == type::java_lang_Character()) {
    return DexMethod::make_method(
        "Ljava/lang/Character;.valueOf:(C)Ljava/lang/Character;");
  } else if (type == type::java_lang_Integer()) {
    return DexMethod::make_method(
        "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;");
  } else if (type == type::java_lang_Long()) {
    return DexMethod::make_method(
        "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;");
  } else if (type == type::java_lang_Float()) {
    return DexMethod::make_method(
        "Ljava/lang/Float;.valueOf:(Z)Ljava/lang/Float;");
  } else if (type == type::java_lang_Double()) {
    return DexMethod::make_method(
        "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;");
  }
  return nullptr;
}

DataType to_datatype(const DexType* t) {
  auto const name = t->get_name()->c_str();
  switch (name[0]) {
  case 'V':
    return DataType::Void;
  case 'Z':
    return DataType::Boolean;
  case 'B':
    return DataType::Byte;
  case 'S':
    return DataType::Short;
  case 'C':
    return DataType::Char;
  case 'I':
    return DataType::Int;
  case 'J':
    return DataType::Long;
  case 'F':
    return DataType::Float;
  case 'D':
    return DataType::Double;
  case 'L':
    return DataType::Object;
  case '[':
    return DataType::Array;
  }
  not_reached();
}

bool is_subclass(const DexType* parent, const DexType* child) {
  auto super = child;
  while (super != nullptr) {
    if (parent == super) return true;
    const auto cls = type_class(super);
    if (cls == nullptr) break;
    super = cls->get_super_class();
  }
  return false;
}

bool is_uninstantiable_class(DexType* type) {
  if (type == nullptr || type::is_array(type) || type::is_primitive(type)) {
    return false;
  }

  if (type == java_lang_Void()) {
    return true;
  }

  auto cls = type_class(type);
  if (cls == nullptr || is_interface(cls) || is_native(cls) ||
      cls->is_external() || !cls->rstate.can_delete()) {
    return false;
  }
  return !cls->has_ctors();
}

}; // namespace type
