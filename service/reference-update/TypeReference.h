/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "ClassHierarchy.h"
#include "ConcurrentContainers.h"
#include "DexClass.h"

using TypeSet = std::set<const DexType*, dextypes_comparator>;
using UnorderedTypeSet = std::unordered_set<const DexType*>;

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
  explicit TypeRefUpdater(
      const std::unordered_map<DexType*, DexType*>& old_to_new);

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

  ConcurrentMap<DexMethod*, DexProto*> m_inits;
  const std::unordered_map<DexType*, DexType*>& m_old_to_new;
};

/**
 * original_name + "$RDX$" + hash_of_signature
 */
const DexString* new_name(const DexMethodRef* method);

const DexString* new_name(const DexFieldRef* field);

// A helper to stringify method signature for the method dedup mapping file.
std::string get_method_signature(const DexMethod* method);

bool proto_has_reference_to(const DexProto* proto,
                            const UnorderedTypeSet& targets);

/**
 * Get a new proto by updating the type references on the proto from an old type
 * to the provided new type.
 */
DexProto* get_new_proto(
    const DexProto* proto,
    const std::unordered_map<const DexType*, DexType*>& old_to_new);

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
    const ClassHierarchy& ch,
    boost::optional<std::unordered_map<DexMethod*, std::string>&>
        method_debug_map = boost::none);

void update_field_type_references(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& old_to_new);

void fix_colliding_dmethods(
    const Scope& scope,
    const std::vector<std::pair<DexMethod*, DexProto*>>& colliding_methods);
} // namespace type_reference
