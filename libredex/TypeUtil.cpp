/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeUtil.h"

#include "DexUtil.h"
#include "RedexContext.h"

namespace type {

#define DEFINE_CACHED_TYPE(func_name, _) \
  DexType* func_name() { return g_redex->pointers_cache().type_##func_name(); }

#define FOR_EACH DEFINE_CACHED_TYPE
WELL_KNOWN_TYPES
#undef FOR_EACH

namespace pseudo {

#define DEFINE_CACHED_PSEUDO_TYPE(func_name, _)           \
  DexFieldRef* func_name() {                              \
    return g_redex->pointers_cache().field_##func_name(); \
  }

#define FOR_EACH DEFINE_CACHED_PSEUDO_TYPE
PRIMITIVE_PSEUDO_TYPE_FIELDS
#undef FOR_EACH

} // namespace pseudo

bool is_valid(const std::string& descriptor) {
  if (descriptor.empty()) {
    return false;
  }
  size_t non_array_start = 0;

  while (non_array_start < descriptor.length() &&
         descriptor[non_array_start] == '[') {
    ++non_array_start;
  }
  if (non_array_start == descriptor.length()) {
    return false;
  }

  switch (descriptor[non_array_start]) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
  case 'V':
    return non_array_start + 1 == descriptor.length(); // Must be last character
  case 'L':
    break;
  default:
    return false;
  }

  // Object type now.

  // Must be at least three characters.
  if (non_array_start + 3 > descriptor.length()) {
    return false;
  }

  // Last one is a semicolon.
  if (descriptor[descriptor.length() - 1] != ';') {
    return false;
  }

  // Scan the identifiers.
  size_t start = non_array_start + 1;
  size_t i = start;
  for (; i != descriptor.length() - 1; ++i) {
    if (descriptor[i] == '/') {
      // Found a segment, do a check.
      if (!is_valid_identifier(descriptor, start, i - start)) {
        return false;
      }
      start = i + 1;
    }
  }
  if (start != i) {
    // Handle tail.
    if (!is_valid_identifier(descriptor, start, i - start)) {
      return false;
    }
  }

  return true;
}

bool is_primitive(const DexType* type) {
  switch (type->get_name()->str().at(0)) {
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
  default:
    not_reached_log("unexpected leading character in type: %s",
                    type->get_name()->c_str());
  }
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
  if (type != nullptr && is_array(type)) {
    if (base_type != nullptr && is_array(base_type)) {
      auto element_type = get_array_element_type(type);
      auto element_base_type = get_array_element_type(base_type);
      if (!is_primitive(element_type) && !is_primitive(element_base_type) &&
          check_cast(element_type, element_base_type)) {
        return true;
      }
    }
    return base_type == java_lang_Object();
  }
  const auto cls = type_class(type);
  if (cls == nullptr) return false;
  if (check_cast(cls->get_super_class(), base_type)) return true;
  auto intfs = cls->get_interfaces();
  for (auto intf : *intfs) {
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
  return java_names::internal_to_simple(type->str());
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
        "Ljava/lang/Float;.valueOf:(F)Ljava/lang/Float;");
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

  auto cls = type_class(type);
  if (cls == nullptr || is_interface(cls) || is_native(cls) ||
      cls->is_external() || !cls->rstate.can_delete()) {
    return false;
  }
  return !cls->has_ctors();
}

boost::optional<int32_t> evaluate_type_check(const DexType* src_type,
                                             const DexType* test_type) {
  if (test_type == src_type) {
    // Trivial.
    return 1;
  }

  // Early optimization: always true for test_type = java.lang.Object.
  if (test_type == java_lang_Object()) {
    return 1;
  }

  auto test_cls = type_class(test_type);
  if (test_cls == nullptr) {
    return boost::none;
  }

  auto src_cls = type_class(src_type);
  if (src_cls == nullptr) {
    return boost::none;
  }

  // OK, let's simplify for now. While some SDK classes should be set in
  // stone, let's only work on internals.
  if (test_cls->is_external() || src_cls->is_external()) {
    return boost::none;
  }

  // Class vs class, for simplicity.
  if (!is_interface(test_cls) && !is_interface(src_cls)) {
    if (type::check_cast(src_cls->get_type(), test_cls->get_type())) {
      // If check-cast succeeds, the result will be `true`.
      return 1;
    } else if (!type::check_cast(test_cls->get_type(), src_cls->get_type())) {
      // The check can never succeed, as the test class is not a subtype.
      return 0;
    }
    return boost::none;
  }

  return boost::none;
}

bool is_kotlin_lambda(const DexClass* cls) {
  DexType* kotlin_type = DexType::make_type("Lkotlin/jvm/internal/Lambda;");
  if (cls->get_super_class() == kotlin_type) {
    return true;
  }
  return false;
}

bool is_kotlin_non_capturing_lambda(const DexClass* cls) {
  if (!is_kotlin_lambda(cls)) {
    return false;
  }

  if (cls->get_ifields().empty()) {
    return true;
  }

  return false;
}

}; // namespace type
