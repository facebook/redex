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

#include "DexClass.h"
#include "IRInstruction.h"
#include "MethodUtil.h"
#include "PassManager.h"
#include "TypeUtil.h"

using TypeVector = std::vector<const DexType*>;

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
 * Looks for a <clinit> method for the given class, creates a new one if it
 * does not exist
 */
DexMethod* get_or_create_clinit(DexClass* cls);

/**
 * Return possible deserializer and serializer classes of the given class
 * 'class$Deserializer;', 'class_Deserializer;', 'class$Serializer;',
 * 'class_Serializer;'
 */
ClassSerdes get_class_serdes(const DexClass* cls);

/**
 * Return true if method signatures (name and proto) match.
 */
inline bool signatures_match(const DexMethodRef* a, const DexMethodRef* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

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
 * Subclass check. Copied from VirtualScope.
 * We can make this much faster in time.
 */
bool is_subclass(const DexType* parent, const DexType* child);

/**
 * Whether the given type refers to a proper class that has no ctor,
 * and is not external or native.
 */
bool is_uninstantiable_class(DexType* type);

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

  int num_unique_strings = 0;
  int num_unique_types = 0;
  int num_unique_protos = 0;
  int num_unique_method_refs = 0;
  int num_unique_field_refs = 0;

  int strings_total_size = 0;
  int types_total_size = 0;
  int protos_total_size = 0;
  int method_refs_total_size = 0;
  int field_refs_total_size = 0;

  int num_dbg_items = 0;
  int dbg_total_size = 0;

  /* Stats collected from the Map List section of a Dex. */
  int header_item_count = 0;
  int header_item_bytes = 0;

  int string_id_count = 0;
  int string_id_bytes = 0;

  int type_id_count = 0;
  int type_id_bytes = 0;

  int proto_id_count = 0;
  int proto_id_bytes = 0;

  int field_id_count = 0;
  int field_id_bytes = 0;

  int method_id_count = 0;
  int method_id_bytes = 0;

  int class_def_count = 0;
  int class_def_bytes = 0;

  int call_site_id_count = 0;
  int call_site_id_bytes = 0;

  int method_handle_count = 0;
  int method_handle_bytes = 0;

  int map_list_count = 0;
  int map_list_bytes = 0;

  int type_list_count = 0;
  int type_list_bytes = 0;

  int annotation_set_ref_list_count = 0;
  int annotation_set_ref_list_bytes = 0;

  int annotation_set_count = 0;
  int annotation_set_bytes = 0;

  int class_data_count = 0;
  int class_data_bytes = 0;

  int code_count = 0;
  int code_bytes = 0;

  int string_data_count = 0;
  int string_data_bytes = 0;

  int debug_info_count = 0;
  int debug_info_bytes = 0;

  int annotation_count = 0;
  int annotation_bytes = 0;

  int encoded_array_count = 0;
  int encoded_array_bytes = 0;

  int annotations_directory_count = 0;
  int annotations_directory_bytes = 0;
};

dex_stats_t& operator+=(dex_stats_t& lhs, const dex_stats_t& rhs);

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
  std::size_t last_dot = nice_name.rfind(".");
  if (last_dot != std::string::npos) {
    return nice_name.substr(0, last_dot);
  } else {
    // something went wrong? let's just return the name
    return nice_name;
  }
}
} // namespace java_names
