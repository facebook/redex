/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "DexClass.h"
#include "PassManager.h"

using TypeVector = std::vector<const DexType*>;

/**
 * Return the DexType for java.lang.Object.
 */
DexType* get_object_type();

/**
 * Return the DexType for a void (V) type.
 */
DexType* get_void_type();

/**
 * Return the DexType for an int (I) type.
 */
DexType* get_int_type();

/**
 * Return the DexType for a long (J) type.
 */
DexType* get_long_type();

/**
 * Return the DexType for a boolean (Z) type.
 */
DexType* get_boolean_type();

/**
 * Return the DexType for a double (D) type.
 */
DexType* get_double_type();

/**
 * Return the DexType for an java.lang.String type.
 */
DexType* get_string_type();

/**
 * Return the DexType for an java.lang.Class type.
 */
DexType* get_class_type();

/**
 * Return the DexType for an java.lang.Enum type.
 */
DexType* get_enum_type();

/**
 * Return true if the type is a primitive.
 */
bool is_primitive(DexType* type);

/**
 * Return true if method signatures (name and proto) match.
 */
inline bool signatures_match(const DexMethod* a, const DexMethod* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

/*
 * Return the shorty char for this type.
 * int -> I
 * bool -> Z
 * ... primitive etc.
 * any reference -> L
 */
char type_shorty(DexType* type);

/**
 * Return true if the parent chain leads to known classes.
 * False if one of the parent is in a scope unknown to redex.
 */
bool has_hierarchy_in_scope(DexClass* cls);

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

/**
 * Return the basic datatype of given DexType.
 */
DataType type_to_datatype(const DexType* t);

/**
 * Return the DexClass that represents the DexType in input or nullptr if
 * no such DexClass exists.
 */
inline DexClass* type_class(const DexType* t) {
  return g_redex->type_class(t);
}

/**
 * Return the DexClass that represents an internal DexType or nullptr if
 * no such DexClass exists.
 */
inline DexClass* type_class_internal(const DexType* t) {
  auto dc = type_class(t);
  if (dc == nullptr || dc->is_external())
    return nullptr;
  return dc;
}

/**
 * Check whether a type can be cast to another type.
 * That is, if 'base_type' is an ancestor or an interface implemented by 'type'.
 * However the check is only within classes known to the app. So
 * you may effectively get false for a check_cast that would succeed at
 * runtime. Otherwise 'true' implies the type can cast.
 */
bool check_cast(DexType* type, DexType* base_type);

/**
 * Return the direct children of a type.
 */
inline const TypeVector& get_children(const DexType* type) {
  return g_redex->get_children(type);
}

/**
 * Return true if the type is an array type.
 */
bool is_array(const DexType* type);

/**
 * Return the level of the array type, that is the number of '[' in the array.
 * int[] => [I
 * int[][] => [[I
 * etc.
 */
uint32_t get_array_level(const DexType* type);

/**
 * Return the type of a given array type or nullptr if the type is not
 * an array.
 */
DexType* get_array_type(const DexType*);

/**
 * Retrieves all the children of a type and pushes them in the provided vector.
 * Effectively everything that derives from a given type.
 * There is no guaranteed or known order in which children are returned.
 */
void get_all_children(const DexType*, TypeVector&);

/**
 * True if the method is a constructor (matches the "<init>" name)
 */
bool is_init(const DexMethod* method);

/**
 * True if the method is a static constructor (matches the "<clinit>" name)
 */
bool is_clinit(const DexMethod* method);

/**
 * Whether the method is a ctor or static ctor.
 */
inline bool is_any_init(const DexMethod* method) {
  return is_init(method) || is_clinit(method);
}

/**
 * Merge the 2 visibility access flags. Return the most permissive visibility.
 */
DexAccessFlags merge_visibility(uint32_t vis1, uint32_t vis2);

/**
 * This is a private implementation to build the type system.
 * Please do not randomly call this function if you are not sure what you
 * are doing.
 */
void build_type_system(DexClass* cls);

/**
 * Sorts and unique-ifies the given vector.
 */
template <class T, class Cmp = std::less<T>>
void sort_unique(std::vector<T>& vec, Cmp cmp = std::less<T>()) {
  std::sort(vec.begin(), vec.end(), cmp);
  auto last = std::unique(vec.begin(), vec.end());
  vec.erase(last, vec.end());
}

/**
 * True if this instruction is passing through all the args of its enclosing
 * method.  This predicate simplifies inlining optimizations since otherwise
 * the optimization would have to re-map the input regs.  The N arguments to
 * the invoke should be the last N registers of the frame.
 */
bool passes_args_through(DexOpcodeMethod* insn,
                         const DexCode& code,
                         int ignore = 0);

/**
 * Generates a Scope& object from a set of Dexes.
 *
 */
template <class T>
Scope build_class_scope(const T& dexen) {
  Scope v;
  for (auto const& classes : dexen) {
    for (auto clazz : classes) {
      v.push_back(clazz);
    }
  }
  return v;
};
Scope build_class_scope(DexStoresVector& stores);

/**
 * Posts the changes made to the Scope& object to the
 * Dexes.
 *
 */
template <class T>
void post_dexen_changes(const Scope& v, T& dexen) {
  std::unordered_set<DexClass*> clookup(v.begin(), v.end());
  for (auto& classes : dexen) {
    classes.erase(
      std::remove_if(
        classes.begin(),
        classes.end(),
        [&](DexClass* cls) {
          return !clookup.count(cls);
        }),
      classes.end());
  }
  if (debug) {
    std::unordered_set<DexClass*> dlookup;
    for (auto const& classes : dexen) {
      for (auto const& cls : classes) {
        dlookup.insert(cls);
      }
    }
    for (auto const& cls : clookup) {
      assert_log(dlookup.count(cls), "Can't add classes in post_dexen_changes");
    }
  }
};
void post_dexen_changes(const Scope& v, DexStoresVector& stores);
