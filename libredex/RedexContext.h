/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <array>
#include <vector>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

#include "DexMemberRefs.h"

class DexDebugInstruction;
class DexString;
class DexType;
class DexField;
class DexTypeList;
class DexProto;
class DexMethod;
class DexClass;
struct DexFieldRef;
struct DexDebugEntry;
struct DexPosition;
struct RedexContext;

extern RedexContext* g_redex;

struct RedexContext {
  RedexContext();
  ~RedexContext();

  DexString* make_string(const char* nstr, uint32_t utfsize);
  DexString* get_string(const char* nstr, uint32_t utfsize);
  template <typename V> void visit_all_dexstring(V v);

  static constexpr size_t kMaxPlaceholderString = 16;
  DexString* get_placeholder_string(size_t index) const;

  DexType* make_type(DexString* dstring);
  DexType* get_type(DexString* dstring);
  void alias_type_name(DexType* type, DexString* new_name);
  template <typename V> void visit_all_dextype(V v);

  DexField* make_field(const DexType* container,
                       const DexString* name,
                       const DexType* type);
  DexField* get_field(const DexType* container,
                      const DexString* name,
                      const DexType* type);
  void mutate_field(DexField* field,
                    const DexFieldRef& ref);

  DexTypeList* make_type_list(std::list<DexType*>&& p);
  DexTypeList* get_type_list(std::list<DexType*>&& p);

  DexProto* make_proto(DexType* rtype,
                       DexTypeList* args,
                       DexString* shorty);
  DexProto* get_proto(DexType* rtype, DexTypeList* args);

  DexMethod* make_method(DexType* type,
                         DexString* name,
                         DexProto* proto);
  DexMethod* get_method(DexType* type,
                        DexString* name,
                        DexProto* proto);
  void erase_method(DexMethod*);
  void mutate_method(DexMethod* method,
                     const DexMethodRef& ref,
                     bool rename_on_collision = false);

  DexDebugEntry* make_dbg_entry(DexDebugInstruction* opcode);
  DexDebugEntry* make_dbg_entry(DexPosition* pos);

  void build_type_system(DexClass*);
  DexClass* type_class(const DexType* t);
  const std::vector<const DexType*>& get_children(const DexType* type);

 private:
  struct carray_cmp {
    bool operator()(const char* a, const char* b) const {
      return (strcmp(a, b) < 0);
    }
  };

  // DexString
  std::map<const char*, DexString*, carray_cmp> s_string_map;
  std::mutex s_string_lock;

  std::array<DexString*, kMaxPlaceholderString> s_placeholder_strings;

  // DexType
  std::map<DexString*, DexType*> s_type_map;
  std::mutex s_type_lock;

  // DexField
  std::unordered_map<DexFieldRef, DexField*> s_field_map;
  std::mutex s_field_lock;

  // DexTypeList
  std::map<std::list<DexType*>, DexTypeList*> s_typelist_map;
  std::mutex s_typelist_lock;

  // DexProto
  std::map<DexType*, std::map<DexTypeList*, DexProto*>> s_proto_map;
  std::mutex s_proto_lock;

  // DexMethod
  std::unordered_map<DexMethodRef, DexMethod*> s_method_map;
  std::mutex s_method_lock;

  // Type-to-class map and class hierarchy
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;
  std::unordered_map<
    const DexType*, std::vector<const DexType*>> m_class_hierarchy;

  const std::vector<const DexType*> m_empty_types;
};

template <typename V>
void RedexContext::visit_all_dexstring(V v) {
  std::lock_guard<std::mutex> lock(s_string_lock);
  for (auto const& p : s_string_map) {
    v(p.second);
  }
}

template <typename V>
void RedexContext::visit_all_dextype(V v) {
  std::lock_guard<std::mutex> lock(s_type_lock);
  for (auto const& p : s_type_map) {
    v(p.second);
  }
}
