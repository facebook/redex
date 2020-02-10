/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <vector>

#include "ClassUtil.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "MethodUtil.h"
#include "PassManager.h"
#include "TypeUtil.h"

/**
 * Change the visibility of members accessed in a method.
 * We make everything public, except if a scope argument is given; then accessed
 * members in the same scope will not be made public (We could be more precise
 * and walks the inheritance hierarchy as needed.)
 */
void change_visibility(DexMethod* method, DexType* scope = nullptr);

void change_visibility(IRCode* code, DexType* scope = nullptr);

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
 */
bool relocate_method_if_no_changes(DexMethod* method, DexType* to_type);

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

namespace java_names {

// Example: "Ljava/lang/String;" --> "java.lang.String"
inline std::string internal_to_external(const std::string& internal_name) {
  auto external_name = internal_name.substr(1, internal_name.size() - 2);
  std::replace(external_name.begin(), external_name.end(), '/', '.');
  return external_name;
}

// Example: "java.lang.String" --> "Ljava/lang/String;"
inline std::string external_to_internal(const std::string& external_name) {
  auto internal_name = "L" + external_name + ";";
  std::replace(internal_name.begin(), internal_name.end(), '.', '/');
  return internal_name;
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
