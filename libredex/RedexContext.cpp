/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RedexContext.h"

#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"

RedexContext* g_redex;

RedexContext::~RedexContext() {
  // Delete DexStrings.
  for (auto const& p : s_string_map) {
    delete p.second;
  }
  // Delete DexTypes.  NB: This table intentionally contains aliases (multiple
  // DexStrings map to the same DexType), so we have to dedup the set of types
  // before deleting to avoid double-frees.
  std::unordered_set<DexType*> delete_types;
  for (auto const& p : s_type_map) {
    delete_types.emplace(p.second);
  }
  for (auto const& t : delete_types) {
    delete t;
  }
  // Delete DexFields.
  for (auto const& p1 : s_field_map) {
    for (auto const& p2 : p1.second) {
      for (auto const& p3 : p2.second) {
        delete p3.second;
      }
    }
  }
  // Delete DexTypeLists.
  for (auto const& p : s_typelist_map) {
    delete p.second;
  }
  // Delete DexProtos.
  for (auto const& p1 : s_proto_map) {
    for (auto const& p2 : p1.second) {
      delete p2.second;
    }
  }
  // Delete DexMethods.
  for (auto const& p1 : s_method_map) {
    for (auto const& p2 : p1.second) {
      for (auto const& p3 : p2.second) {
        delete p3.second;
      }
    }
  }
}

DexString* RedexContext::make_string(const char* nstr, uint32_t utfsize) {
  always_assert(nstr != nullptr);
  DexString* rv;
  pthread_mutex_lock(&s_string_lock);
  if (s_string_map.count(nstr) == 0) {
    rv = new DexString(nstr, utfsize);
    s_string_map.emplace(rv->m_cstr, rv);
    pthread_mutex_unlock(&s_string_lock);
    return rv;
  }
  rv = s_string_map.at(nstr);
  pthread_mutex_unlock(&s_string_lock);
  return rv;
}

DexString* RedexContext::get_string(const char* nstr, uint32_t utfsize) {
  if (nstr == nullptr) {
    return nullptr;
  }
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  pthread_mutex_lock(&s_string_lock);
  auto find = s_string_map.find(nstr);
  auto result = find != s_string_map.end() ? find->second : nullptr;
  pthread_mutex_unlock(&s_string_lock);
  return result;
}

DexType* RedexContext::make_type(DexString* dstring) {
  always_assert(dstring != nullptr);
  DexType* rv;
  pthread_mutex_lock(&s_type_lock);
  if (s_type_map.count(dstring) == 0) {
    rv = new DexType(dstring);
    s_type_map.emplace(dstring, rv);
    pthread_mutex_unlock(&s_type_lock);
    return rv;
  }
  rv = s_type_map.at(dstring);
  pthread_mutex_unlock(&s_type_lock);
  return rv;
}

DexType* RedexContext::get_type(DexString* dstring) {
  if (dstring == nullptr) {
    return nullptr;
  }
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  pthread_mutex_lock(&s_type_lock);
  auto find = s_type_map.find(dstring);
  auto result = find != s_type_map.end() ? find->second : nullptr;
  pthread_mutex_unlock(&s_type_lock);
  return result;
}

void RedexContext::alias_type_name(DexType* type, DexString* new_name) {
  pthread_mutex_lock(&s_type_lock);
  always_assert_log(!s_type_map.count(new_name),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  type->m_name = new_name;
  s_type_map.emplace(new_name, type);
  pthread_mutex_unlock(&s_type_lock);
}

DexField* RedexContext::make_field(DexType* container,
                                   DexString* name,
                                   DexType* type) {
  always_assert(container != nullptr && name != nullptr && type != nullptr);
  DexField* rv;
  pthread_mutex_lock(&s_field_lock);
  if (s_field_map[container][name].count(type) == 0) {
    rv = new DexField(container, name, type);
    s_field_map[container][name][type] = rv;
    pthread_mutex_unlock(&s_field_lock);
    return rv;
  }
  rv = s_field_map[container][name][type];
  pthread_mutex_unlock(&s_field_lock);
  return rv;
}

DexField* RedexContext::get_field(DexType* container,
                                  DexString* name,
                                  DexType* type) {
  if (container == nullptr || name == nullptr || type == nullptr) {
    return nullptr;
  }
  // Still need to perform the locking in case a make_method call on another
  // thread is modifying the map.
  pthread_mutex_lock(&s_field_lock);
  if (s_field_map[container][name].count(type) == 0) {
    pthread_mutex_unlock(&s_field_lock);
    return nullptr;
  }
  DexField* rv = s_field_map[container][name][type];
  pthread_mutex_unlock(&s_field_lock);
  return rv;
}

DexTypeList* RedexContext::make_type_list(std::list<DexType*>&& p) {
  DexTypeList* rv;
  pthread_mutex_lock(&s_typelist_lock);
  if (s_typelist_map.count(p) == 0) {
    rv = new DexTypeList(std::move(p));
    s_typelist_map[rv->m_list] = rv;
    pthread_mutex_unlock(&s_typelist_lock);
    return rv;
  }
  rv = s_typelist_map.at(p);
  pthread_mutex_unlock(&s_typelist_lock);
  return rv;
}

DexTypeList* RedexContext::get_type_list(std::list<DexType*>&& p) {
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  pthread_mutex_lock(&s_typelist_lock);
  auto find = s_typelist_map.find(p);
  auto result = find != s_typelist_map.end() ? find->second : nullptr;
  pthread_mutex_unlock(&s_typelist_lock);
  return result;
}

DexProto* RedexContext::make_proto(DexType* rtype,
                                   DexTypeList* args,
                                   DexString* shorty) {
  always_assert(rtype != nullptr && args != nullptr && shorty != nullptr);
  DexProto* rv;
  pthread_mutex_lock(&s_proto_lock);
  if (s_proto_map[rtype].count(args) == 0) {
    rv = new DexProto(rtype, args, shorty);
    s_proto_map[rtype][args] = rv;
    pthread_mutex_unlock(&s_proto_lock);
    return rv;
  }
  rv = s_proto_map[rtype][args];
  pthread_mutex_unlock(&s_proto_lock);
  return rv;
}

DexProto* RedexContext::get_proto(DexType* rtype, DexTypeList* args) {
  if (rtype == nullptr || args == nullptr) {
    return nullptr;
  }
  DexProto* rv;
  pthread_mutex_lock(&s_proto_lock);
  if (s_proto_map[rtype].count(args) == 0) {
    pthread_mutex_unlock(&s_proto_lock);
    return nullptr;
  }
  rv = s_proto_map[rtype][args];
  pthread_mutex_unlock(&s_proto_lock);
  return rv;
}

DexMethod* RedexContext::make_method(DexType* type,
                                     DexString* name,
                                     DexProto* proto) {
  always_assert(type != nullptr && name != nullptr && proto != nullptr);
  DexMethod* rv;
  pthread_mutex_lock(&s_method_lock);
  if (s_method_map[type][name].count(proto) == 0) {
    rv = new DexMethod(type, name, proto);
    s_method_map[type][name][proto] = rv;
    pthread_mutex_unlock(&s_method_lock);
    return rv;
  }
  rv = s_method_map[type][name][proto];
  pthread_mutex_unlock(&s_method_lock);
  return rv;
}

DexMethod* RedexContext::get_method(DexType* type,
                                    DexString* name,
                                    DexProto* proto) {
  if (type == nullptr || name == nullptr || proto == nullptr) {
    return nullptr;
  }
  // Still need to perform the locking in case a make_method call on another
  // thread is modifying the map.
  pthread_mutex_lock(&s_method_lock);
  if (s_method_map[type][name].count(proto) == 0) {
    pthread_mutex_unlock(&s_method_lock);
    return nullptr;
  }
  DexMethod* rv = s_method_map[type][name][proto];
  pthread_mutex_unlock(&s_method_lock);
  return rv;
}

void RedexContext::mutate_method_class(DexMethod* method, DexType* cls) {
  pthread_mutex_lock(&s_method_lock);
  s_method_map[method->m_class][method->m_name].erase(method->m_proto);
  method->m_class = cls;
  s_method_map[method->m_class][method->m_name][method->m_proto] = method;
  pthread_mutex_unlock(&s_method_lock);
}

void RedexContext::mutate_method_proto(DexMethod* method, DexProto* proto) {
  pthread_mutex_lock(&s_method_lock);
  s_method_map[method->m_class][method->m_name].erase(method->m_proto);
  method->m_proto = proto;
  s_method_map[method->m_class][method->m_name][method->m_proto] = method;
  pthread_mutex_unlock(&s_method_lock);
}
