/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "DexClass.h"

/**
 * When we rename dex type or replace an old type with a new type, we may need
 * update corresponding string literals in dex instructions and
 * dalvik.annotation.Signature annotations.
 */

namespace rewriter {

class TypeStringMap {
  UnorderedMap<const DexString*, const DexString*> m_type_name_map;

 public:
  TypeStringMap() {}
  explicit TypeStringMap(
      const UnorderedMap<const DexType*, DexType*>& type_mapping);
  /**
   * Add type mapping from old_name to new_name.
   */
  void add_type_name(const DexString* old_name, const DexString* new_name);
  /**
   * Get a new type name for the old type name, return null if the old type name
   * does not exist in the mapping. Array types are supported properly.
   */
  const DexString* get_new_type_name(const DexString* old_name) const;
  const UnorderedMap<const DexString*, const DexString*>& get_class_map()
      const {
    return m_type_name_map;
  }
};

/**
 * dalvik.annotation.Signature annotations store class names as strings, when we
 * rename these classes, we should update the strings properly at the same time.
 */
void rewrite_dalvik_annotation_signature(const Scope& scope,
                                         const TypeStringMap& mapping);

/**
 * Rewrite string literals in instructions from old type names to new type
 * names. Return the number of total updates.
 *
 * const-string "com.facebook.TypeXYZ" => const-string "X.A"
 */
uint32_t rewrite_string_literal_instructions(const Scope& scope,
                                             const TypeStringMap& mapping);

} // namespace rewriter
