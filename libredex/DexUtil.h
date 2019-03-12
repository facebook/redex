/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <vector>
#include <unordered_set>

#include "DexClass.h"
#include "IRInstruction.h"
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
 * Return the DexType for a byte (B) type.
 */
DexType* get_byte_type();

/**
 * Return the DexType for a char (C) type.
 */
DexType* get_char_type();

/**
 * Return the DexType for a short (S) type.
 */
DexType* get_short_type();

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
 * Return the DexType for a float (F) type.
 */
DexType* get_float_type();

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
 * Return the DexType for an java.lang.Integer type.
 */
DexType* get_integer_type();

struct ClassSerdes {
  std::vector<DexType*> serdes;

  ClassSerdes(DexType* deser,
              DexType* flatbuf_deser,
              DexType* ser,
              DexType* flatbuf_ser)
      : serdes{deser, flatbuf_deser, ser, flatbuf_ser} {}

  std::vector<DexType*> get_all_serdes() { return serdes; }

  DexType* get_deser() { return serdes[0]; }
  DexType* get_flatbuf_deser() { return serdes[1]; }
  DexType* get_ser() { return serdes[2]; }
  DexType* get_flatbuf_ser() { return serdes[3]; }
};

/**
 * Return possible deserializer and serializer classes of the given class
 * 'class$Deserializer;', 'class_Deserializer;', 'class$Serializer;',
 * 'class_Serializer;'
 */
ClassSerdes get_class_serdes(const DexClass* cls);

/**
 * Return the DexType for an java.lang.Throwable type.
 */
DexType* get_throwable_type();

/**
 * Return the package for a valid DexType.
 */
std::string get_package_name(const DexType* type);

/**
 * Return the simple name w/o the package name and the ending ';' for a valid
 * DexType. E.g., 'Lcom/facebook/Simple;' -> 'Simple'.
 */
std::string get_simple_name(const DexType* type);

/**
 * Return true if the type is a primitive.
 */
bool is_primitive(const DexType* type);

/**
 * Return true if the type is either a long or a double
 */
bool is_wide_type(const DexType* type);

/**
 * Return true if method signatures (name and proto) match.
 */
inline bool signatures_match(const DexMethodRef* a, const DexMethodRef* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

/*
 * Return the shorty char for this type.
 * int -> I
 * bool -> Z
 * ... primitive etc.
 * any reference -> L
 */
char type_shorty(const DexType* type);

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
 * Check whether a type can be cast to another type.
 * That is, if 'base_type' is an ancestor or an interface implemented by 'type'.
 * However the check is only within classes known to the app. So
 * you may effectively get false for a check_cast that would succeed at
 * runtime. Otherwise 'true' implies the type can cast.
 */
bool check_cast(const DexType* type, const DexType* base_type);

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
bool is_integer(const DexType* type);

bool is_boolean(const DexType* type);

bool is_long(const DexType* type);

bool is_float(const DexType* type);

bool is_double(const DexType* type);

bool is_void(const DexType* type);

/**
 * Return the level of the array type, that is the number of '[' in the array.
 * int[] => [I
 * int[][] => [[I
 * etc.
 */
uint32_t get_array_level(const DexType* type);

/**
 * Return the type of a given array type or the type itself if it's not an array
 *
 * Examples:
 *   [java.lang.String -> java.lang.String
 *   java.lang.Integer -> java.lang.Integer
 */
const DexType* get_array_type_or_self(const DexType*);

/**
 * Return the type of a given array type or nullptr if the type is not
 * an array.
 */
DexType* get_array_type(const DexType*);

/**
 * According to the description in the Java 7 spec:
 * https://docs.oracle.com/javase/specs/jls/se7/html/jls-10.html Given an array
 * type, it's components are referenced directly via array access expressions
 * that use integer index values. The component type of an array may itself be
 * an array type.
 */
DexType* get_array_component_type(const DexType*);

/**
 * Return the array type of a given type.
 */
DexType* make_array_type(const DexType*);

/**
 * Return the array type of a given type in specified level.
 */
DexType* make_array_type(const DexType*, uint32_t level);

/**
 * True if the method is a constructor (matches the "<init>" name)
 */
bool is_init(const DexMethodRef* method);

/**
 * True if the method is a static constructor (matches the "<clinit>" name)
 */
bool is_clinit(const DexMethodRef* method);

/**
 * Whether the method is a ctor or static ctor.
 */
inline bool is_any_init(const DexMethodRef* method) {
  return is_init(method) || is_clinit(method);
}

/**
 * Subclass check. Copied from VirtualScope.
 * We can make this much faster in time.
 */
bool is_subclass(const DexType* parent, const DexType* child);

/**
 * Change the visibility of members accessed in a method.
 * We make everything public but we could be more precise and only
 * relax visibility as needed.
 */
void change_visibility(DexMethod* method);

/**
 * NOTE: Only relocates the method. Doesn't check the correctness here,
 *       nor does it make sure that the members are accessible from the
 *       new type.
 */
void relocate_method(DexMethod* method, DexType* to_type);

/**
 * Checks if a method can be relocated, i.e. if it doesn't require any changes
 * to the referenced methods (none of the referenced methods would need to
 * change into a virtual / static method).
 */
bool no_changes_when_relocating_method(const DexMethod* method);

/**
 * Check that the method contains no invoke-super instruction; this is a
 * requirement to relocate a method outside of its original inheritance
 * hierarchy.
 */
bool no_invoke_super(const DexMethod* method);

/**
 * Relocates the method only if relocate_method_if_no_changes returns true.
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

void load_root_dexen(DexStore& store,
                     const std::string& dexen_dir_str,
                     bool balloon = false,
                     bool verbose = true,
                     bool support_dex_v37 = false);

/**
 * Creates a generated store based on the given classes.
 *
 * NOTE: InterDex will take care of adding the classes to the root store.
 * TODO: Add a way to define a real store.
 */
void create_store(const std::string& store_name,
                  DexStoresVector& stores,
                  DexClasses classes);

/*
 * This exists because in the absence of a register allocator, we need each
 * transformation to keep the ins registers at the end of the frame. Once the
 * register allocator is switched on this function should no longer have many
 * use cases.
 */
size_t sum_param_sizes(const IRCode*);

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

struct dex_stats_t {
  int num_types = 0;
  int num_classes = 0;
  int num_methods = 0;
  int num_method_refs = 0;
  int num_fields = 0;
  int num_field_refs = 0;
  int num_strings = 0;
  int num_protos = 0;
  int num_static_values = 0;
  int num_annotations = 0;
  int num_type_lists = 0;
  int num_bytes = 0;
  int num_instructions = 0;
};

dex_stats_t&
  operator+=(dex_stats_t& lhs, const dex_stats_t& rhs);

namespace JavaNameUtil {

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
  std::size_t last_dot = nice_name.rfind(".");
  if (last_dot != std::string::npos) {
    return nice_name.substr(0, last_dot);
  } else {
    // something went wrong? let's just return the name
    return nice_name;
  }
}
}
