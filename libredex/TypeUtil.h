/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "WellKnownTypes.h"

/**
 * Basic datatypes used by bytecode.
 */
enum class DataType : uint8_t {
  Void,
  Boolean,
  Byte,
  Short,
  Char,
  Int,
  Long,
  Float,
  Double,
  Object,
  Array
};

namespace type {

#define DECLARE_TYPE(name, _) DexType* name();

#define FOR_EACH DECLARE_TYPE
WELL_KNOWN_TYPES
#undef FOR_EACH
#undef DECLARE_TYPE

namespace pseudo {
#define DECLARE_PSEUDO_TYPE_FIELD(name, _) DexFieldRef* name();

#define FOR_EACH DECLARE_PSEUDO_TYPE_FIELD
PRIMITIVE_PSEUDO_TYPE_FIELDS
#undef FOR_EACH
} // namespace pseudo

// Do some simple checks to ascertain whether the descriptor looks valid.
// NOTE: may fail for UTF strings.
bool is_valid(std::string_view descriptor);

/**
 * Return true if the type is a primitive.
 */
bool is_primitive(const DexType* type);

/**
 * Return true if the type is either a long or a double
 */
bool is_wide_type(const DexType* type);

/**
 * Return true if the type is an array type.
 */
bool is_array(const DexType* type);

/**
 * Return true if the type is an object type (array types included).
 */
bool is_object(const DexType* type);

/**
 * Return true if the type is a primitive type that fits within a 32-bit
 * register, i.e., boolean, byte, char, short or int.
 */
bool is_integral(const DexType* type);

bool is_int(const DexType* type);

bool is_char(const DexType* type);

bool is_short(const DexType* type);

bool is_boolean(const DexType* type);

bool is_byte(const DexType* type);

bool is_long(const DexType* type);

bool is_float(const DexType* type);

bool is_double(const DexType* type);

bool is_void(const DexType* type);

bool is_java_lang_object_array(const DexType* type);

bool is_reference_array(const DexType* type);

/*
 * Return the shorty char for this type.
 * int -> I
 * bool -> Z
 * ... primitive etc.
 * any reference -> L
 */
char type_shorty(const DexType* type);

/**
 * Check whether a type can be cast to another type.
 * That is, if 'base_type' is an ancestor or an interface implemented by 'type'.
 * However the check is only within classes known to the app. So
 * you may effectively get false for a check_cast that would succeed at
 * runtime. Otherwise 'true' implies the type can cast.
 */
bool check_cast(const DexType* type, const DexType* base_type);

/**
 * Return the package for a valid DexType.
 */
std::string_view get_package_name(const DexType* type);

/**
 * Check if the two types are from the same package.
 */
bool same_package(const DexType* type1, const DexType* type2);

/**
 * Return the simple name w/o the package name and the ending ';' for a valid
 * DexType. E.g., 'Lcom/facebook/Simple;' -> 'Simple'.
 */
std::string get_simple_name(const DexType* type);

/**
 * Return the level of the array type, that is the number of '[' in the array.
 * int[] => [I
 * int[][] => [[I
 * etc.
 */
uint32_t get_array_level(const DexType* type);

/**
 * The component type of an array is the type of the values contained in the
 * array. E.g.:
 *
 * [LFoo; -> LFoo;
 * [[LFoo; -> [LFoo;
 */
DexType* get_array_component_type(const DexType* type);

/**
 * An array's component type may also be an array. Recursively unwrapping these
 * array types will give us the element type. E.g.:
 *
 * [LFoo; -> LFoo;
 * [[LFoo; -> LFoo;
 *
 * If the input argument is not an array type, this returns null.
 *
 * The terms "component type" and "element type" are defined in the JLS:
 * https://docs.oracle.com/javase/specs/jls/se7/html/jls-10.html
 */
DexType* get_array_element_type(const DexType* type);

/**
 * Return the element type of a given array type or the type itself if it's not
 * an array.
 *
 * Examples:
 *   [java.lang.String -> java.lang.String
 *   java.lang.Integer -> java.lang.Integer
 */
const DexType* get_element_type_if_array(const DexType* type);

/**
 * Return the (level 1) array type of a given type.
 */
DexType* make_array_type(const DexType* type);

/**
 * Return the array type of a given type in specified level.
 */
DexType* make_array_type(const DexType* type, uint32_t level);

/**
 * Returns the corresponding wrapper type of primitive types
 * e.g.
 *   I -> Ljava/lang/Integer;
 *   Z -> Ljava/lang/Boolean;
 *   ... etc.
 * returns nullptr if argument `type` is not a primitive type or is void
 */
DexType* get_boxed_reference_type(const DexType* type);

DexMethodRef* get_unboxing_method_for_type(const DexType* type);

/* Takes a reference type, returns the Number(i.e. abstract) method of its
 * corresponding unboxing method. Notes: this is only used for detecting
 * boxing/unboxing pattern in CSE. Once we can resolve Ljava/lang/Number;.* to
 * its proper impls in each callsite, this function can be removed.
 */
DexMethodRef* get_Number_unboxing_method_for_type(const DexType* type);

DexMethodRef* get_value_of_method_for_type(const DexType* type);

/**
 * Return the basic datatype of given DexType.
 */
DataType to_datatype(const DexType* t);

/**
 * Subclass check. Copied from VirtualScope.
 * We can make this much faster in time.
 */
bool is_subclass(const DexType* parent, const DexType* child);

/**
 * Whether the given type refers to a proper class that has no ctor,
 * and is not external or native. This function only makes a quick determination
 * without considering whether an interface or abstract class has any
 * implementations (see the RemoveUninstantiablesPass for a more complete
 * analysis).
 */
bool is_uninstantiable_class(DexType* type);

/**
 * Evaluate a type check on the `src_type` against the `test_type`. It is
 * equivalent to the semantic of the INSTANCE_OF check. If the check passes, the
 * function returns 1; if it fails, the function returns 0. If it cannot be
 * determined, the function returns none.
 */
boost::optional<int32_t> evaluate_type_check(const DexType* src_type,
                                             const DexType* test_type);

/**
 * Validate if the caller has the permit to call a method or access a field.
 *
 * +-------------------------+--------+----------+-----------+-------+
 * | Access Levels Modifier  | Class  | Package  | Subclass  | World |
 * +-------------------------+--------+----------+-----------+-------+
 * | public                  | Y      | Y        | Y         | Y     |
 * | protected               | Y      | Y        | Y         | N     |
 * | no modifier             | Y      | Y        | N         | N     |
 * | private                 | Y      | N        | N         | N     |
 * +-------------------------+--------+----------+-----------+-------+
 */
template <typename DexMember>
bool can_access(const DexMethod* accessor, const DexMember* accessee) {
  if (accessee == nullptr) {
    // In case the accessee is nullptr, we return true. Since blocking nullptr
    // is not the intention of this check.
    return true;
  }
  auto accessor_class = accessor->get_class();
  if (is_public(accessee) || accessor_class == accessee->get_class()) {
    return true;
  }
  if (is_private(accessee)) {
    // Cannot be same class
    return false;
  }
  auto accessee_class = accessee->get_class();
  auto from_same_package = same_package(accessor_class, accessee_class);
  redex_assert(is_protected(accessee) || is_package_private(accessee));
  if (from_same_package) {
    return true;
  }
  if (is_protected(accessee) && check_cast(accessor_class, accessee_class)) {
    return true;
  }
  return false;
}
template <>
inline bool can_access(const DexMethod* accessor, const DexClass* accessee) {
  if (accessee == nullptr) {
    // In case the accessee is nullptr, we return true. Since blocking nullptr
    // is not the intention of this check.
    return true;
  }
  auto accessor_class = accessor->get_class();
  if (is_public(accessee) || accessor_class == accessee->get_type()) {
    return true;
  }
  if (is_private(accessee)) {
    // Cannot be same class
    return false;
  }
  return true;
}

/**
 * Return true if the cls is derived from Kotlin lambda.
 */
bool is_kotlin_lambda(const DexClass* cls);

/*
 * Return true if the cls is kotlin non capturing lambda.
 */
bool is_kotlin_non_capturing_lambda(const DexClass* cls);

}; // namespace type
