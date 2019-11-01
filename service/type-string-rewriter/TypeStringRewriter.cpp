/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeStringRewriter.h"

#include "Walkers.h"

namespace {

/* In Signature annotations, parameterized types of the form Foo<Bar> get
 * represented as the strings
 *   "Lcom/baz/Foo" "<" "Lcom/baz/Bar;" ">"
 *   or
 *   "Lcom/baz/Foo<" "Lcom/baz/Bar;" ">"
 *
 * Note that "Lcom/baz/Foo" lacks a trailing semicolon.
 * Signature annotations suck.
 *
 * This method transforms the input to the form expected by the alias map:
 *   "Lcom/baz/Foo;"
 * looks that up in the map, then transforms back to the form of the input.
 */
DexString* lookup_signature_annotation(const rewriter::TypeStringMap& mapping,
                                       DexString* anno) {
  bool has_bracket = false;
  bool added_semicolon = false;
  std::string anno_str = anno->str();
  // anno_str looks like one of these
  // Lcom/baz/Foo<
  // Lcom/baz/Foo;
  // Lcom/baz/Foo
  if (anno_str.back() == '<') {
    anno_str.pop_back();
    has_bracket = true;
  }
  // anno_str looks like one of these
  // Lcom/baz/Foo;
  // Lcom/baz/Foo
  if (anno_str.back() != ';') {
    anno_str.push_back(';');
    added_semicolon = true;
  }
  // anno_str looks like this
  // Lcom/baz/Foo;

  // Use get_string because if it's in the map, then it must also already exist
  DexString* transformed_anno = DexString::get_string(anno_str);
  if (transformed_anno == nullptr) {
    return nullptr;
  }
  auto obfu = mapping.get_new_type_name(transformed_anno);
  if (obfu) {
    if (!added_semicolon && !has_bracket) {
      return obfu;
    }
    std::string obfu_str = obfu->str();
    // We need to transform back to the old_name format of the input
    if (added_semicolon) {
      always_assert(obfu_str.back() == ';');
      obfu_str.pop_back();
    }
    if (has_bracket) {
      always_assert(obfu_str.back() != '<');
      obfu_str.push_back('<');
    }
    return DexString::make_string(obfu_str);
  }
  return nullptr;
}

uint32_t get_array_level(const DexString* name) {
  uint32_t level = 0;
  auto str = name->c_str();
  while (*str++ == '[') {
  }
  return level;
}

DexString* make_array(const DexString* name, uint32_t level) {
  std::string array;
  array.reserve(name->size() + level + 1);
  array.append(level, '[');
  array.append(name->str());
  return DexString::make_string(array);
}
} // namespace

namespace rewriter {

void TypeStringMap::add_type_name(DexString* old_name, DexString* new_name) {
  always_assert(old_name && new_name);
  m_type_name_map[old_name] = new_name;
  if (old_name->str()[0] != '[') {
    return;
  }
  // If old_name is an array, we store the mapping for the component type of the
  // arrays.
  uint32_t old_level = get_array_level(old_name);
  uint32_t new_level = get_array_level(new_name);
  always_assert(old_level == new_level);
  old_name = DexString::make_string(old_name->c_str() + old_level);
  new_name = DexString::make_string(new_name->c_str() + new_level);
  m_type_name_map[old_name] = new_name;
}

DexString* TypeStringMap::get_new_type_name(DexString* old_name) const {
  auto it = m_type_name_map.find(old_name);
  if (it != m_type_name_map.end()) {
    return it->second;
  }
  auto level = get_array_level(old_name);
  if (level == 0) {
    return nullptr;
  }
  old_name = DexString::get_string(old_name->c_str() + level);
  if (old_name == nullptr) {
    return nullptr;
  }
  it = m_type_name_map.find(old_name);
  if (it != m_type_name_map.end()) {
    return make_array(it->second, level);
  }
  return nullptr;
}

void rewrite_dalvik_annotation_signature(const Scope& scope,
                                         const TypeStringMap& mapping) {
  static DexType* dalviksig =
      DexType::get_type("Ldalvik/annotation/Signature;");
  walk::parallel::annotations(scope, [&](DexAnnotation* anno) {
    if (anno->type() != dalviksig) return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      auto ev = elem.encoded_value;
      if (ev->evtype() != DEVT_ARRAY) continue;
      auto arrayev = static_cast<DexEncodedValueArray*>(ev);
      auto const& evs = arrayev->evalues();
      for (auto strev : *evs) {
        if (strev->evtype() != DEVT_STRING) continue;
        auto stringev = static_cast<DexEncodedValueString*>(strev);
        DexString* old_str = stringev->string();
        DexString* new_str = lookup_signature_annotation(mapping, old_str);
        if (new_str != nullptr) {
          TRACE(RENAME, 5, "Rewriting Signature from '%s' to '%s'",
                old_str->c_str(), new_str->c_str());
          stringev->string(new_str);
        }
      }
    }
  });
}
} // namespace rewriter
