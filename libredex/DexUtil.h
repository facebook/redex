/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <functional>
#include <unordered_set>
#include <vector>

#include "ClassUtil.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRInstruction.h"
#include "MethodUtil.h"
#include "TypeUtil.h"

/**
 * Change the visibility of members accessed in a method.
 * We make everything public, except if a scope argument is given; then accessed
 * members in the same scope will not be made public (We could be more precise
 * and walk the inheritance hierarchy as needed.)
 */
void change_visibility(DexMethod* method, DexType* scope = nullptr);

void change_visibility(IRCode* code,
                       DexType* scope,
                       DexMethod* effective_caller_resolved_from);

/**
 * NOTE: Only relocates the method. Doesn't check the correctness here,
 *       nor does it make sure that the members are accessible from the
 *       new type.
 */
void relocate_method(DexMethod* method, DexType* to_type);

/**
 * Checks if a method can be relocated, i.e. if it doesn't require any changes
 * to invoked direct methods (none of the invoked direct methods would need to
 * change into a public virtual / static method) or framework protected methods.
 * Any problematic invoked methods are added to the optionally supplied set.
 */
bool gather_invoked_methods_that_prevent_relocation(
    const DexMethod* method,
    std::unordered_set<DexMethodRef*>* methods_preventing_relocation = nullptr);

/**
 * Relocates the method only if
 * gather_invoked_methods_that_prevent_relocation returns true.
 * It also updates the visibility of the accessed members.
 * NOTE: Does not check can_change_visibility_for_relocation.
 * TODO: Consider integrating the full visibility check in
 * gather_invoked_methods_that_prevent_relocation.
 */
bool relocate_method_if_no_changes(DexMethod* method, DexType* to_type);

/**
 * Checks if visibility can be changed via change_visibility in a way that is
 * suitable for relocate_method.
 */
bool can_change_visibility_for_relocation(const DexMethod* method);

bool can_change_visibility_for_relocation(
    const IRCode* code, const DexMethod* effective_caller_resolved_from);

/**
 * Merge the 2 visibility access flags. Return the most permissive visibility.
 */
DexAccessFlags merge_visibility(uint32_t vis1, uint32_t vis2);

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
bool passes_args_through(IRInstruction* insn,
                         const IRCode& code,
                         int ignore = 0);

/**
 * Creates a runtime exception block of instructions. This is primarily used
 * by transformations for substituting instructions which throw an exception
 * at runtime. Currently, used for substituting switch case instructions.
 */
void create_runtime_exception_block(DexString* except_str,
                                    std::vector<IRInstruction*>& block);

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
Scope build_class_scope(const DexStoresVector& stores);

Scope build_class_scope_for_packages(
    const DexStoresVector& stores,
    const std::unordered_set<std::string>& package_names);

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
        std::remove_if(classes.begin(),
                       classes.end(),
                       [&](DexClass* cls) { return !clookup.count(cls); }),
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

void load_root_dexen(DexStore& store,
                     const std::string& dexen_dir_str,
                     bool balloon = false,
                     bool verbose = true,
                     int support_dex_version = 35);

/**
 * Creates a generated store based on the given classes.
 *
 * NOTE: InterDex will take care of adding the classes to the root store.
 * TODO: Add a way to define a real store.
 */
void create_store(const std::string& store_name,
                  DexStoresVector& stores,
                  DexClasses classes);

/**
 * Determine if the given dex item has the given annotation
 *
 * @param t The dex item whose annotations we'll examine
 * @param anno_type The annotation we're looking for, expressed as DexType
 * @return true IFF dex item t is annotated with anno_type
 */
template <typename T>
bool has_anno(const T* t, const DexType* anno_type) {
  if (anno_type == nullptr) return false;
  if (t->get_anno_set() == nullptr) return false;
  for (const auto& anno : t->get_anno_set()->get_annotations()) {
    if (anno->type() == anno_type) {
      return true;
    }
  }
  return false;
}

template <typename T>
bool has_anno(const T* t, const std::unordered_set<DexType*>& anno_types) {
  if (t->get_anno_set() == nullptr) return false;
  for (const auto& anno : t->get_anno_set()->get_annotations()) {
    if (anno_types.count(anno->type())) {
      return true;
    }
  }
  return false;
}

// Check whether the given string is a valid identifier. This does
// not handle UTF. Checks against the Java bytecode specification,
// which is a bit more relaxed than Dex's.
bool is_valid_identifier(const std::string& s, size_t start, size_t len);
bool is_valid_identifier(const std::string& s);

namespace java_names {

inline boost::optional<std::string> primitive_desc_to_name(char desc) {
  const static std::unordered_map<char, std::string> conversion_table{
      {'V', "void"},    {'B', "byte"},  {'C', "char"},
      {'S', "short"},   {'I', "int"},   {'J', "long"},
      {'Z', "boolean"}, {'F', "float"}, {'D', "double"},
  };
  auto it = conversion_table.find(desc);
  if (it != conversion_table.end()) {
    return it->second;
  } else {
    return boost::none;
  }
}

inline boost::optional<char> primitive_name_to_desc(const std::string& name) {
  const static std::unordered_map<std::string, char> conversion_table{
      {"void", 'V'},    {"byte", 'B'},  {"char", 'C'},
      {"short", 'S'},   {"int", 'I'},   {"long", 'J'},
      {"boolean", 'Z'}, {"float", 'F'}, {"double", 'D'},
  };
  auto it = conversion_table.find(name);
  if (it != conversion_table.end()) {
    return it->second;
  } else {
    return boost::none;
  }
}

// Example: "Ljava/lang/String;" --> "java.lang.String"
// Example: "[Ljava/lang/String;" --> "[Ljava.lang.String;"
// Example: "I" --> "int"
// Example: "[I" --> "[I"
inline std::string internal_to_external(const std::string& internal_name) {
  int array_level = std::count(internal_name.begin(), internal_name.end(), '[');

  std::string component_name = internal_name.substr(array_level);

  char type = component_name.at(0);
  if (type == 'L') {
    // For arrays, we need to preserve the semicolon at the end of the name
    auto external_name = component_name.substr(
        1, component_name.size() - (array_level == 0 ? 2 : 1));
    std::replace(external_name.begin(), external_name.end(), '/', '.');
    std::string array_prefix;
    array_prefix.reserve(array_level);
    for (int i = 0; i < array_level; i++) {
      array_prefix += '[';
    }
    if (array_level != 0) {
      array_prefix += "L"; // external only uses 'L' for arrays
    }
    return array_prefix + external_name;
  } else if (array_level) {
    // If the type is an array of primitives, the external format is the same
    // as internal.
    return internal_name;
  } else {
    auto maybe_external_name = primitive_desc_to_name(type);
    always_assert_log(
        maybe_external_name, "%c is not a valid primitive type.", type);
    return *maybe_external_name;
  }
}

// Example: "java.lang.String" --> "Ljava/lang/String;"
// Example: "[Ljava.lang.String;" --> "[Ljava/lang/String;"
// Example: "int" --> "I"
// Example: "[I" --> "[I"
// Example: "I" --> "LI;"
// Example: "[LI;" --> "[LI;"
inline std::string external_to_internal(const std::string& external_name) {
  // Primitive types (not including their arrays) are special notations
  auto maybe_primitive_name = primitive_name_to_desc(external_name);
  if (maybe_primitive_name) {
    return std::string(1, *maybe_primitive_name);
  }

  int array_level = std::count(external_name.begin(), external_name.end(), '[');
  std::string component_external_name = external_name.substr(array_level);
  /**
   * Note: "I" is a perfectly valid external name denoting a class of "LI;"
   * while "int" is the external name for int type. However, "[I" is an array of
   * int. For an array of "I", you need to use "[LI;"
   */
  if (array_level != 0 && component_external_name.size() == 1) {
    // It must be an array of primitives. The internal name is the same as the
    // external name.
    return external_name;
  }

  std::string component_internal_name = component_external_name;
  if (array_level == 0) {
    component_internal_name = "L" + component_internal_name;
  }

  std::replace(
      component_internal_name.begin(), component_internal_name.end(), '.', '/');
  if (!boost::algorithm::ends_with(component_internal_name, ";")) {
    component_internal_name += ";";
  }
  std::string array_prefix;
  array_prefix.reserve(array_level);
  for (int i = 0; i < array_level; i++) {
    array_prefix += '[';
  }
  return array_prefix + component_internal_name;
}

// Example: "Ljava/lang/String;" --> "String"
// Example: "[Ljava/lang/String;" --> "String[]"
// Example: "I" --> "int"
// Example: "[I" --> "int[]"
// Example: "LA$B$C;" --> "C"
// Example: "[LA$B;" --> "B[]"
// Example: "Ljava/lang$1;" --> ""
// Note: kotlin anonymous class is not handled properly here.
inline std::string internal_to_simple(const std::string& internal_name) {
  int array_level = std::count(internal_name.begin(), internal_name.end(), '[');
  std::string component_name = internal_name.substr(array_level);
  std::string component_external_name = internal_to_external(component_name);
  std::size_t last_dot = component_external_name.rfind('.');
  std::size_t last_dollar = component_external_name.rfind('$');
  std::string component_simple_name;
  if (last_dot == std::string::npos && last_dollar == std::string::npos) {
    component_simple_name = component_external_name;
  } else if (last_dot == std::string::npos) {
    component_simple_name = component_external_name.substr(last_dollar + 1);
  } else if (last_dollar == std::string::npos) {
    component_simple_name = component_external_name.substr(last_dot + 1);
  } else {
    size_t simple_begin = (last_dot < last_dollar) ? last_dollar : last_dot;
    component_simple_name = component_external_name.substr(simple_begin + 1);
  }
  if (std::all_of(component_simple_name.begin(),
                  component_simple_name.end(),
                  isdigit)) {
    component_simple_name = "";
  }
  // append a pair of [] for each array level.
  std::string array_suffix;
  array_suffix.reserve(2 * array_level);
  for (int i = 0; i < array_level; i++) {
    array_suffix += "[]";
  }
  return component_simple_name + array_suffix;
}

inline std::string package_name(const std::string& type_name) {
  std::string nice_name = internal_to_external(type_name);
  std::size_t last_dot = nice_name.rfind('.');
  if (last_dot != std::string::npos) {
    return nice_name.substr(0, last_dot);
  } else {
    // something went wrong? let's just return the name
    return nice_name;
  }
}
} // namespace java_names
