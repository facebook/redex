/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

using TypeSet = std::set<const DexType*, dextypes_comparator>;

namespace type_reference {

/**
 * Update old type reference to new type reference in all the fields and methods
 * in the scope, but is not responsible for updating opcodes. The users should
 * take care of other part of analysis and transformations to make sure the
 * updating being valid. This supports updating virtual methods through name
 * mangling instead of walking through virtual scopes.
 * Usage examples:
 *    1. Replace candidate enum types with Integer type after we finish the code
 *       transformation.
 *    2. Replace interfaces or parent classes references with new type
 *       references after we merge them to their single implementation or single
 *       child classes.
 * If the original name of a method or a field is "member_name", the updated
 * name may be "member_name$RDX$some_hash_value".
 */
class TypeRefUpdater final {
 public:
  /**
   * The old types should all have definitions so that it's unlikely that we are
   * trying to update a virtual method that may override any external virtual
   * method.
   */
  TypeRefUpdater(const std::unordered_map<DexType*, DexType*>& old_to_new);

  void update_methods_fields(const Scope& scope);

 private:
  /**
   * Try to convert "type" to a new type. Return nullptr if it's not found in
   * the old_to_new mapping. LOld; => LNew; [LOld; => [LNew;
   * [[LOld; => [[LNew;
   * ...
   */
  DexType* try_convert_to_new_type(DexType* type);
  /**
   * Change a field to new type if its original type is a candidate.
   * Return true if the field is updated.
   */
  bool mangling(DexFieldRef* field);
  /**
   * Change proto of a method if its proto contains any candidate.
   */
  bool mangling(DexMethodRef* method);
  /**
   * org_name + m_mangling_affix + seed
   */
  DexString* gen_new_name(const std::string& org_name, size_t seed);

  const std::unordered_map<DexType*, DexType*>& m_old_to_new;
  const std::string m_mangling_affix = "$RDX$";
};

// A helper to stringify method signature for the method dedup mapping file.
std::string get_method_signature(const DexMethod* method);

bool proto_has_reference_to(const DexProto* proto, const TypeSet& targets);

/**
 * Update the proto in-place using the old_to_new map. Here we update the type
 * references on the proto from an old type to the provided new type.
 */
DexProto* update_proto_reference(
    const DexProto* proto,
    const std::unordered_map<const DexType*, DexType*>& old_to_new);

/**
 * Helper functions building a modified DexTypeList by appending or prepending
 * elements.
 */
DexTypeList* prepend_and_make(const DexTypeList* list, DexType* new_type);

DexTypeList* append_and_make(const DexTypeList* list, DexType* new_type);

DexTypeList* append_and_make(const DexTypeList* list,
                             const std::vector<DexType*>& new_types);

DexTypeList* replace_head_and_make(const DexTypeList* list, DexType* new_head);

DexTypeList* drop_and_make(const DexTypeList* list, size_t num_types_to_drop);

/**
 * Update all method signature type references in-place using the old_to_new
 * map. We update all references to an old type to the provided new type.
 *
 * The optional `method_debug_map` stores the map from the updated DexMethod to
 * the string representation of the original method signature.
 */
void update_method_signature_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new,
    boost::optional<std::unordered_map<DexMethod*, std::string>&>
        method_debug_map = boost::none);

void update_field_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new);

} // namespace type_reference
