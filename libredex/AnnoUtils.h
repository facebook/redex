/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"

#include <unordered_set>

/**
 * Parses the default value of an annotation given the annotation and the
 * element name.
 *
 * For any annotation Foo, the default values for the annotation elements are
 * stored under a system annotation, namely dalvik.annotation.AnnotationDefault,
 * in Foo. The default values are stored under this annotation with the element
 * name "value" and have the value type VALUE_ANNOTATION.
 *
 * See
 * https://source.android.com/devices/tech/dalvik/dex-format#dalvik-annotation-default
 * for more details on annotaion defaults encoding.
 *
 * Params
 *   target_anno_type - DexType of anno for which default is being looked up
 *   lookup_anno_element_name - The element name which is being lookedup
 *
 * Return
 *   The DexEncodedValue of the default value. Caller has to appropriately
 *   extract the value from the encoded result. See integ test for examples
 */
const DexEncodedValue* parse_default_anno_value(
    const DexType* target_anno, const std::string& anno_element_name);

bool parse_bool_anno_value(const DexMethod* method,
                           const DexType* target_anno,
                           std::string name = "");

uint32_t parse_int_anno_value(const DexMethod* method,
                              const DexType* target_anno,
                              std::string name = "");

uint32_t parse_int_anno_value(const DexClass* cls,
                              const DexType* target_anno,
                              std::string name = "");

std::string parse_str_anno_value(const DexMethod* method,
                                 const DexType* target_anno,
                                 std::string name = "");

template <class DexMember>
bool has_attribute(DexMember* member,
                   const DexType* target_anno,
                   const std::string& attr_name) {
  auto& annos = member->get_anno_set()->get_annotations();
  for (auto& anno : annos) {
    if (anno->type() != target_anno) {
      continue;
    }
    for (auto& elem : anno->anno_elems()) {
      if (strcmp(elem.string->c_str(), attr_name.c_str()) == 0) {
        return true;
      }
    }
  }

  return false;
}

template <class DexMember>
DexAnnotation* get_annotation(const DexMember* member, DexType* anno_type) {
  const auto& annos = member->get_anno_set();
  if (annos == nullptr) {
    return nullptr;
  }
  for (auto& anno : annos->get_annotations()) {
    if (anno->type() == anno_type) {
      return anno;
    }
  }
  return nullptr;
}

template <class DexMember>
bool has_any_annotation(DexMember* member,
                        const std::unordered_set<DexType*>& anno_types) {
  const auto& annos = member->get_anno_set();
  if (annos == nullptr) {
    return false;
  }
  for (auto& anno : annos->get_annotations()) {
    if (anno_types.count(anno->type())) {
      return true;
    }
  }
  return false;
}

DexAnnotationSet* create_anno_set(
    const std::vector<std::pair<std::string, std::string>>& elements,
    DexType* anno_type);
