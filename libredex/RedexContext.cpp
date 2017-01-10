/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RedexContext.h"

#include <mutex>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"

RedexContext* g_redex;

RedexContext::RedexContext() {
  for (size_t i = 0; i < kMaxPlaceholderString; ++i) {
    s_placeholder_strings[i] = DexString::make_placeholder();
  }
}

RedexContext::~RedexContext() {
  // Delete DexStrings.
  for (auto const& p : s_string_map) {
    delete p.second;
  }
  for (auto const& s : s_placeholder_strings) {
    delete s;
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
  for (auto const& it : s_field_map) {
    delete it.second;
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
  for (auto const& it : s_method_map) {
    delete it.second;
  }
}

DexString* RedexContext::make_string(const char* nstr, uint32_t utfsize) {
  always_assert(nstr != nullptr);
  std::lock_guard<std::mutex> lock(s_string_lock);
  auto it = s_string_map.find(nstr);
  if (it == s_string_map.end()) {
    auto rv = new DexString(nstr, utfsize);
    s_string_map.emplace(rv->m_cstr, rv);
    return rv;
  } else {
    return it->second;
  }
}

DexString* RedexContext::get_string(const char* nstr, uint32_t utfsize) {
  if (nstr == nullptr) {
    return nullptr;
  }
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  std::lock_guard<std::mutex> lock(s_string_lock);
  auto find = s_string_map.find(nstr);
  auto result = find != s_string_map.end() ? find->second : nullptr;
  return result;
}

DexString* RedexContext::get_placeholder_string(size_t index) const {
  always_assert(index < kMaxPlaceholderString);
  return s_placeholder_strings[index];
}

DexType* RedexContext::make_type(DexString* dstring) {
  always_assert(dstring != nullptr);
  std::lock_guard<std::mutex> lock(s_type_lock);
  auto it = s_type_map.find(dstring);
  if (it == s_type_map.end()) {
    auto rv = new DexType(dstring);
    s_type_map.emplace(dstring, rv);
    return rv;
  } else {
    return it->second;
  }
}

DexType* RedexContext::get_type(DexString* dstring) {
  if (dstring == nullptr) {
    return nullptr;
  }
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  std::lock_guard<std::mutex> lock(s_type_lock);
  auto find = s_type_map.find(dstring);
  auto result = find != s_type_map.end() ? find->second : nullptr;
  return result;
}

void RedexContext::alias_type_name(DexType* type, DexString* new_name) {
  std::lock_guard<std::mutex> lock(s_type_lock);
  always_assert_log(!s_type_map.count(new_name),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  type->m_name = new_name;
  s_type_map.emplace(new_name, type);
}

DexField* RedexContext::make_field(const DexType* container,
                                   const DexString* name,
                                   const DexType* type) {
  always_assert(container != nullptr && name != nullptr && type != nullptr);
  std::lock_guard<std::mutex> lock(s_field_lock);
  DexFieldRef r(const_cast<DexType*>(container),
                const_cast<DexString*>(name),
                const_cast<DexType*>(type));
  auto it = s_field_map.find(r);
  if (it == s_field_map.end()) {
    auto rv = new DexField(const_cast<DexType*>(container),
                      const_cast<DexString*>(name),
                      const_cast<DexType*>(type));
    s_field_map.emplace(r, rv);
    return rv;
  } else {
    return it->second;
  }
}

DexField* RedexContext::get_field(const DexType* container,
                                  const DexString* name,
                                  const DexType* type) {
  if (container == nullptr || name == nullptr || type == nullptr) {
    return nullptr;
  }
  DexFieldRef r(const_cast<DexType*>(container),
                const_cast<DexString*>(name),
                const_cast<DexType*>(type));
  // Still need to perform the locking in case a make_method call on another
  // thread is modifying the map.
  std::lock_guard<std::mutex> lock(s_field_lock);
  auto it = s_field_map.find(r);
  if (it == s_field_map.end()) {
    return nullptr;
  } else {
    return it->second;
  }
}

void RedexContext::mutate_field(DexField* field,
                                const DexFieldRef& ref) {
  std::lock_guard<std::mutex> lock(s_field_lock);
  DexFieldRef& r = field->m_ref;
  s_field_map.erase(r);
  r.cls = ref.cls != nullptr ? ref.cls : field->m_ref.cls;
  r.name = ref.name != nullptr ? ref.name : field->m_ref.name;
  r.type = ref.type != nullptr ? ref.type : field->m_ref.type;
  field->m_ref = r;
  s_field_map.emplace(r, field);
}

DexTypeList* RedexContext::make_type_list(std::list<DexType*>&& p) {
  std::lock_guard<std::mutex> lock(s_typelist_lock);
  auto it = s_typelist_map.find(p);
  if (it == s_typelist_map.end()) {
    auto rv = new DexTypeList(std::move(p));
    s_typelist_map[rv->m_list] = rv;
    return rv;
  } else {
    return it->second;
  }
}

DexTypeList* RedexContext::get_type_list(std::list<DexType*>&& p) {
  // We need to use the lock to prevent undefined behavior if this method is
  // called while the map is being modified.
  std::lock_guard<std::mutex> lock(s_typelist_lock);
  auto find = s_typelist_map.find(p);
  auto result = find != s_typelist_map.end() ? find->second : nullptr;
  return result;
}

DexProto* RedexContext::make_proto(DexType* rtype,
                                   DexTypeList* args,
                                   DexString* shorty) {
  always_assert(rtype != nullptr && args != nullptr && shorty != nullptr);
  std::lock_guard<std::mutex> lock(s_proto_lock);
  if (s_proto_map[rtype].count(args) == 0) {
    auto rv = new DexProto(rtype, args, shorty);
    s_proto_map[rtype][args] = rv;
    return rv;
  }
  return s_proto_map[rtype][args];
}

DexProto* RedexContext::get_proto(DexType* rtype, DexTypeList* args) {
  if (rtype == nullptr || args == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(s_proto_lock);
  if (s_proto_map[rtype].count(args) == 0) {
    return nullptr;
  }
  return s_proto_map[rtype][args];
}

DexMethod* RedexContext::make_method(DexType* type,
                                     DexString* name,
                                     DexProto* proto) {
  always_assert(type != nullptr && name != nullptr && proto != nullptr);
  DexMethodRef r(type, name, proto);
  std::lock_guard<std::mutex> lock(s_method_lock);
  auto it = s_method_map.find(r);
  if (it == s_method_map.end()) {
    auto rv = new DexMethod(type, name, proto);
    s_method_map.emplace(r, rv);
    return rv;
  } else {
    return it->second;
  }
}

DexMethod* RedexContext::get_method(DexType* type,
                                    DexString* name,
                                    DexProto* proto) {
  if (type == nullptr || name == nullptr || proto == nullptr) {
    return nullptr;
  }
  DexMethodRef r(type, name, proto);
  // Still need to perform the locking in case a make_method call on another
  // thread is modifying the map.
  std::lock_guard<std::mutex> lock(s_method_lock);
  auto it = s_method_map.find(r);
  if (it == s_method_map.end()) {
    return nullptr;
  } else {
    return it->second;
  }
}

void RedexContext::erase_method(DexMethod* method) {
  std::lock_guard<std::mutex> lock(s_method_lock);
  s_method_map.erase(method->m_ref);
}

void RedexContext::mutate_method(DexMethod* method,
                                 const DexMethodRef& ref,
                                 bool rename_on_collision /* = false */) {
  std::lock_guard<std::mutex> lock(s_method_lock);
  DexMethodRef& r = method->m_ref;
  s_method_map.erase(r);

  r.cls = ref.cls != nullptr ? ref.cls : method->m_ref.cls;
  r.name = ref.name != nullptr ? ref.name : method->m_ref.name;
  r.proto = ref.proto != nullptr ? ref.proto : method->m_ref.proto;
  if (s_method_map.find(r) != s_method_map.end() && rename_on_collision) {
    std::string original_name(r.name->c_str());
    for (uint16_t i = 0; i < 1000; ++i) {
      r.name = DexString::make_string(
          (original_name + "$redex" + std::to_string(i)).c_str());
      if (s_method_map.find(r) == s_method_map.end()) {
        break;
      }
    }
  }
  always_assert_log(s_method_map.find(r) == s_method_map.end(),
                    "Another method of the same signature already exists");
  s_method_map.emplace(r, method);
}

void RedexContext::build_type_system(DexClass* cls) {
  std::lock_guard<std::mutex> l(m_type_system_mutex);
  const DexType* type = cls->get_type();
  m_type_to_class.emplace(type, cls);
  const auto& super = cls->get_super_class();
  if (super) m_class_hierarchy[super].push_back(type);
}

DexClass* RedexContext::type_class(const DexType* t) {
  auto it = m_type_to_class.find(t);
  return it != m_type_to_class.end() ? it->second : nullptr;
}

const std::vector<const DexType*>& RedexContext::get_children(
  const DexType* type
) {
  const auto& it = m_class_hierarchy.find(type);
  return it != m_class_hierarchy.end() ? it->second : m_empty_types;
}
