/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RedexContext.h"

#include <exception>
#include <mutex>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"

RedexContext* g_redex;

RedexContext::RedexContext() {}

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
  for (auto const& it : s_field_map) {
    delete static_cast<DexField*>(it.second);
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
    // note DexStrings are keyed by the c_str() of the underlying std::string
    // The c_str is valid until a the string is destroyed, or until a non-const
    // function is called on the string (but note the std::string itself is
    // const)
    auto rv = new DexString(nstr, utfsize);
    s_string_map.emplace(rv->c_str(), rv);
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
  always_assert_log(
      !s_type_map.count(new_name),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  type->m_name = new_name;
  s_type_map.emplace(new_name, type);
}

DexFieldRef* RedexContext::make_field(const DexType* container,
                                      const DexString* name,
                                      const DexType* type) {
  always_assert(container != nullptr && name != nullptr && type != nullptr);
  std::lock_guard<std::mutex> lock(s_field_lock);
  DexFieldSpec r(const_cast<DexType*>(container),
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

DexFieldRef* RedexContext::get_field(const DexType* container,
                                     const DexString* name,
                                     const DexType* type) {
  if (container == nullptr || name == nullptr || type == nullptr) {
    return nullptr;
  }
  DexFieldSpec r(const_cast<DexType*>(container),
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

void RedexContext::erase_field(DexFieldRef* field) {
  std::lock_guard<std::mutex> lock(s_field_lock);
  s_field_map.erase(field->m_spec);
}

void RedexContext::mutate_field(
    DexFieldRef* field, const DexFieldSpec& ref, bool rename_on_collision) {
  std::lock_guard<std::mutex> lock(s_field_lock);
  DexFieldSpec& r = field->m_spec;
  s_field_map.erase(r);
  r.cls = ref.cls != nullptr ? ref.cls : field->m_spec.cls;
  r.name = ref.name != nullptr ? ref.name : field->m_spec.name;
  r.type = ref.type != nullptr ? ref.type : field->m_spec.type;
  field->m_spec = r;

  if (rename_on_collision && s_field_map.find(r) != s_field_map.end()) {
    uint32_t i = 0;
    while (true) {
      r.name = DexString::make_string(
          ("f$" + std::to_string(i++)).c_str());
      if (s_field_map.find(r) == s_field_map.end()) {
        break;
      }
    }
  }
  always_assert_log(s_field_map.find(r) == s_field_map.end(),
                    "Another field with the same signature already exists %s",
                    SHOW(s_field_map[r]));
  s_field_map.emplace(r, field);
}

DexTypeList* RedexContext::make_type_list(std::deque<DexType*>&& p) {
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

DexTypeList* RedexContext::get_type_list(std::deque<DexType*>&& p) {
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

DexMethodRef* RedexContext::make_method(DexType* type,
                                        DexString* name,
                                        DexProto* proto) {
  always_assert(type != nullptr && name != nullptr && proto != nullptr);
  DexMethodSpec r(type, name, proto);
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

DexMethodRef* RedexContext::get_method(DexType* type,
                                       DexString* name,
                                       DexProto* proto) {
  if (type == nullptr || name == nullptr || proto == nullptr) {
    return nullptr;
  }
  DexMethodSpec r(type, name, proto);
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

void RedexContext::erase_method(DexMethodRef* method) {
  std::lock_guard<std::mutex> lock(s_method_lock);
  s_method_map.erase(method->m_spec);
}

void RedexContext::mutate_method(DexMethodRef* method,
                                 const DexMethodSpec& ref,
                                 bool rename_on_collision /* = false */) {
  std::lock_guard<std::mutex> lock(s_method_lock);
  DexMethodSpec& r = method->m_spec;
  s_method_map.erase(r);

  r.cls = ref.cls != nullptr ? ref.cls : method->m_spec.cls;
  r.name = ref.name != nullptr ? ref.name : method->m_spec.name;
  r.proto = ref.proto != nullptr ? ref.proto : method->m_spec.proto;
  if (s_method_map.find(r) != s_method_map.end() && rename_on_collision) {
    std::string original_name(r.name->c_str());
    uint32_t i = 0;
    while (true) {
      r.name = DexString::make_string(
          ("r$" + std::to_string(i++)).c_str());
      if (s_method_map.find(r) == s_method_map.end()) {
        break;
      }
    }
  }
  always_assert_log(s_method_map.find(r) == s_method_map.end(),
                    "Another method of the same signature already exists");
  s_method_map.emplace(r, method);
}

void RedexContext::publish_class(DexClass* cls) {
  std::lock_guard<std::mutex> l(m_type_system_mutex);
  const DexType* type = cls->get_type();
  if (m_type_to_class.find(type) != end(m_type_to_class)) {
    const auto& prev_loc = m_type_to_class[type]->get_dex_location();
    const auto& cur_loc = cls->get_dex_location();
    if (prev_loc == cur_loc) {
      TRACE(MAIN, 1, "Warning: found a duplicate class: %s\n", SHOW(cls));
    } else {
      std::string class_name = show(cls);
      std::string dex_1 = m_type_to_class[type]->get_dex_location();
      std::string dex_2 = cls->get_dex_location();

      TRACE(MAIN,
            1,
            "ABORT! Found a duplicate class: %s in two dexes:\ndex 1: %s\ndex "
            "2: %s\n",
            class_name.c_str(),
            dex_1.c_str(),
            dex_2.c_str());

      throw malformed_dex(class_name, dex_1, dex_2);
    }
  }
  m_type_to_class.emplace(type, cls);
}

DexClass* RedexContext::type_class(const DexType* t) {
  auto it = m_type_to_class.find(t);
  return it != m_type_to_class.end() ? it->second : nullptr;
}
