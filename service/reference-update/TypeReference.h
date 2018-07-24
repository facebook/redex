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
