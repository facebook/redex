/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <vector>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

class DexDebugInstruction;
class DexString;
class DexType;
class DexField;
class DexTypeList;
class DexProto;
class DexMethod;
class DexClass;
struct DexFieldRef;
struct DexMethodRef;
struct DexDebugEntry;
struct DexPosition;
struct RedexContext;

extern RedexContext* g_redex;

struct RedexContext {
  RedexContext()
      : s_string_lock(PTHREAD_MUTEX_INITIALIZER),
        s_type_lock(PTHREAD_MUTEX_INITIALIZER),
        s_field_lock(PTHREAD_MUTEX_INITIALIZER),
        s_typelist_lock(PTHREAD_MUTEX_INITIALIZER),
        s_proto_lock(PTHREAD_MUTEX_INITIALIZER),
        s_method_lock(PTHREAD_MUTEX_INITIALIZER)
    {}

  ~RedexContext();

  DexString* make_string(const char* nstr, uint32_t utfsize);
  DexString* get_string(const char* nstr, uint32_t utfsize);
  template <typename V> void visit_all_dexstring(V v);

  DexType* make_type(DexString* dstring);
  DexType* get_type(DexString* dstring);
  void alias_type_name(DexType* type, DexString* new_name);
  template <typename V> void visit_all_dextype(V v);

  DexField* make_field(DexType* container,
                       DexString* name,
                       DexType* type);
  DexField* get_field(DexType* container,
                      DexString* name,
                      DexType* type);
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
  pthread_mutex_t s_string_lock;

  // DexType
  std::map<DexString*, DexType*> s_type_map;
  pthread_mutex_t s_type_lock;

  // DexField
  std::map<DexType*, std::map<DexString*, std::map<DexType*, DexField*>>>
    s_field_map;
  pthread_mutex_t s_field_lock;

  // DexTypeList
  std::map<std::list<DexType*>, DexTypeList*> s_typelist_map;
  pthread_mutex_t s_typelist_lock;

  // DexProto
  std::map<DexType*, std::map<DexTypeList*, DexProto*>> s_proto_map;
  pthread_mutex_t s_proto_lock;

  // DexMethod
  std::map<DexType*, std::map<DexString*, std::map<DexProto*, DexMethod*>>>
      s_method_map;
  pthread_mutex_t s_method_lock;

  // Type-to-class map and class hierarchy
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;
  std::unordered_map<
    const DexType*, std::vector<const DexType*>> m_class_hierarchy;

  const std::vector<const DexType*> m_empty_types;
};

template <typename V>
void RedexContext::visit_all_dexstring(V v) {
  pthread_mutex_lock(&s_string_lock);
  for (auto const& p : s_string_map) {
    v(p.second);
  }
  pthread_mutex_unlock(&s_string_lock);
}

template <typename V>
void RedexContext::visit_all_dextype(V v) {
  pthread_mutex_lock(&s_type_lock);
  for (auto const& p : s_type_map) {
    v(p.second);
  }
  pthread_mutex_unlock(&s_type_lock);
}
