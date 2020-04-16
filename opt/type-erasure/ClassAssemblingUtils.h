/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Creators.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "ModelMethodMerger.h"

struct ModelSpec;
using TypeSet = std::set<const DexType*, dextypes_comparator>;
using FieldsMap = std::unordered_map<const DexType*, std::vector<DexField*>>;

constexpr const char* INTERNAL_TYPE_TAG_FIELD_NAME = "$t";
constexpr const char* EXTERNAL_TYPE_TAG_FIELD_NAME = "mTypeTag";

std::vector<DexField*> create_merger_fields(
    const DexType* owner, const std::vector<DexField*>& mergeable_fields);

void cook_merger_fields_lookup(
    const std::vector<DexField*>& new_fields,
    const FieldsMap& fields_map,
    std::unordered_map<DexField*, DexField*>& merger_fields_lookup);

DexClass* create_class(const DexType* type,
                       const DexType* super_type,
                       const std::string& pkg_name,
                       const std::vector<DexField*>& fields,
                       const TypeSet& interfaces,
                       bool with_default_ctor = false,
                       DexAccessFlags access = ACC_PUBLIC);

DexClass* create_merger_class(const DexType* type,
                              const DexType* super_type,
                              const std::vector<DexField*>& merger_fields,
                              const TypeSet& interfaces,
                              const bool add_type_tag_field,
                              bool with_default_ctor = false);

void patch_iput(const IRList::iterator& it);

void patch_iget(DexMethod* meth,
                const IRList::iterator& it,
                DexType* original_field_type);

void add_class(DexClass* new_cls, Scope& scope, DexStoresVector& stores);

void handle_interface_as_root(ModelSpec& spec,
                              Scope& scope,
                              DexStoresVector& stores);
