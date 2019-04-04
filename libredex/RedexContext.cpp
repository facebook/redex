/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexContext.h"

#include <exception>
#include <mutex>
#include <regex>
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
  for (auto const& p : s_proto_map) {
    delete p.second;
  }
  // Delete DexMethods.
  for (auto const& it : s_method_map) {
    delete static_cast<DexMethod*>(it.second);
  }
  // Delete DexClasses.
  for (auto const& it : m_type_to_class) {
    delete it.second;
  }

  for (const auto& p : s_keep_reasons) {
    delete p.second;
  }
}

/*
 * Try and insert (:key, :value) into :container. This insertion may fail if
 * another thread has already inserted that key. In that case, return the
 * existing value and discard the one we were trying to insert.
 *
 * We distinguish between the types of the inserted and stored values to handle
 * DexFields and DexMethods, where we upcast the inserted value into a
 * DexFieldRef / DexMethodRef respectively when storing it.
 */
template <class InsertValue,
          class StoredValue = InsertValue,
          class Deleter = std::default_delete<InsertValue>,
          class Key,
          class Container>
static StoredValue* try_insert(Key key,
                               InsertValue* value,
                               Container* container) {
  std::unique_ptr<InsertValue, Deleter> to_insert(value);
  if (container->emplace(key, to_insert.get())) {
    return to_insert.release();
  }
  return container->at(key);
}

DexString* RedexContext::make_string(const char* nstr, uint32_t utfsize) {
  always_assert(nstr != nullptr);
  auto rv = s_string_map.get(nstr, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  // Note that DexStrings are keyed by the c_str() of the underlying
  // std::string. The c_str is valid until a the string is destroyed, or until a
  // non-const function is called on the string (but note the std::string itself
  // is const)
  auto dexstring = new DexString(nstr, utfsize);
  return try_insert(dexstring->c_str(), dexstring, &s_string_map);
}

DexString* RedexContext::get_string(const char* nstr, uint32_t utfsize) {
  if (nstr == nullptr) {
    return nullptr;
  }
  return s_string_map.get(nstr, nullptr);
}

DexType* RedexContext::make_type(DexString* dstring) {
  always_assert(dstring != nullptr);
  auto rv = s_type_map.get(dstring, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  return try_insert(dstring, new DexType(dstring), &s_type_map);
}

DexType* RedexContext::get_type(DexString* dstring) {
  if (dstring == nullptr) {
    return nullptr;
  }
  return s_type_map.get(dstring, nullptr);
}

void RedexContext::set_type_name(DexType* type, DexString* new_name) {
  alias_type_name(type, new_name);
  type->m_name = new_name;
}

void RedexContext::alias_type_name(DexType* type, DexString* new_name) {
  always_assert_log(
      !s_type_map.count(new_name),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  s_type_map.emplace(new_name, type);
}

void RedexContext::remove_type_name(DexString* name) { s_type_map.erase(name); }

DexFieldRef* RedexContext::make_field(const DexType* container,
                                      const DexString* name,
                                      const DexType* type) {
  always_assert(container != nullptr && name != nullptr && type != nullptr);
  DexFieldSpec r(const_cast<DexType*>(container),
                 const_cast<DexString*>(name),
                 const_cast<DexType*>(type));
  auto rv = s_field_map.get(r, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  auto field = new DexField(const_cast<DexType*>(container),
                            const_cast<DexString*>(name),
                            const_cast<DexType*>(type));
  return try_insert<DexField, DexFieldRef>(r, field, &s_field_map);
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
  return s_field_map.get(r, nullptr);
}

void RedexContext::erase_field(DexFieldRef* field) {
  s_field_map.erase(field->m_spec);
}

void RedexContext::mutate_field(DexFieldRef* field,
                                const DexFieldSpec& ref,
                                bool rename_on_collision,
                                bool update_deobfuscated_name) {
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
      r.name = DexString::make_string(("f$" + std::to_string(i++)).c_str());
      if (s_field_map.find(r) == s_field_map.end()) {
        break;
      }
    }
  }
  always_assert_log(s_field_map.find(r) == s_field_map.end(),
                    "Another field with the same signature already exists %s",
                    SHOW(s_field_map.at(r)));
  s_field_map.emplace(r, field);

  if (field->is_def() && update_deobfuscated_name) {
    static_cast<DexField*>(field)->set_deobfuscated_name(show(field));
  }
}

DexTypeList* RedexContext::make_type_list(std::deque<DexType*>&& p) {
  auto rv = s_typelist_map.get(p, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  auto typelist = new DexTypeList(std::move(p));
  return try_insert(typelist->m_list, typelist, &s_typelist_map);
}

DexTypeList* RedexContext::get_type_list(std::deque<DexType*>&& p) {
  return s_typelist_map.get(p, nullptr);
}

DexProto* RedexContext::make_proto(DexType* rtype,
                                   DexTypeList* args,
                                   DexString* shorty) {
  always_assert(rtype != nullptr && args != nullptr && shorty != nullptr);
  ProtoKey key(rtype, args);
  auto rv = s_proto_map.get(key, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  return try_insert(key, new DexProto(rtype, args, shorty), &s_proto_map);
}

DexProto* RedexContext::get_proto(DexType* rtype, DexTypeList* args) {
  if (rtype == nullptr || args == nullptr) {
    return nullptr;
  }
  return s_proto_map.get(ProtoKey(rtype, args), nullptr);
}

DexMethodRef* RedexContext::make_method(DexType* type,
                                        DexString* name,
                                        DexProto* proto) {
  always_assert(type != nullptr && name != nullptr && proto != nullptr);
  DexMethodSpec r(type, name, proto);
  auto rv = s_method_map.get(r, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  return try_insert<DexMethod, DexMethodRef, DexMethod::Deleter>(
      r, new DexMethod(type, name, proto), &s_method_map);
}

DexMethodRef* RedexContext::get_method(DexType* type,
                                       DexString* name,
                                       DexProto* proto) {
  if (type == nullptr || name == nullptr || proto == nullptr) {
    return nullptr;
  }
  DexMethodSpec r(type, name, proto);
  return s_method_map.get(r, nullptr);
}

void RedexContext::erase_method(DexMethodRef* method) {
  s_method_map.erase(method->m_spec);
}

void RedexContext::mutate_method(DexMethodRef* method,
                                 const DexMethodSpec& new_spec,
                                 bool rename_on_collision,
                                 bool update_deobfuscated_name) {
  std::lock_guard<std::mutex> lock(s_method_lock);
  DexMethodSpec old_spec = method->m_spec;
  s_method_map.erase(method->m_spec);

  DexMethodSpec& r = method->m_spec;
  r.cls = new_spec.cls != nullptr ? new_spec.cls : method->m_spec.cls;
  r.name = new_spec.name != nullptr ? new_spec.name : method->m_spec.name;
  r.proto = new_spec.proto != nullptr ? new_spec.proto : method->m_spec.proto;

  if (s_method_map.count(r) && rename_on_collision) {
    // Never rename constructors, which causes runtime verification error:
    // "Method 42(Foo;.$init$$0) is marked constructor, but doesn't match name"
    always_assert_log(
        show(r.name) != "<init>" && show(r.name) != "<clinit>",
        "you should not rename constructor on a collision, %s.%s:%s exists",
        SHOW(r.cls), SHOW(r.name), SHOW(r.proto));
    if (new_spec.cls == nullptr) {
      // Either method prototype or name is going to be changed, and we hit a
      // collision. Make an unique name: "name$[0-9]+". But in case of <clinit>,
      // libdex rejects a name like "<clinit>$1". See:
      // http://androidxref.com/9.0.0_r3/xref/dalvik/libdex/DexUtf.cpp#115
      // Valid characters can be found here: [_a-zA-Z0-9$\-]
      // http://androidxref.com/9.0.0_r3/xref/dalvik/libdex/DexUtf.cpp#50
      // If a method name begins with "<", it must end with ">". We generate a
      // name like "$clinit$$42" by replacing <, > with $.
      uint32_t i = 0;
      std::string prefix;
      if (r.name->str().front() == '<') {
        redex_assert(r.name->str().back() == '>');
        prefix =
            "$" + r.name->str().substr(1, r.name->str().length() - 2) + "$$";
      } else {
        prefix = r.name->str() + "$";
      }
      do {
        r.name = DexString::make_string((prefix + std::to_string(i++)).c_str());
      } while (s_method_map.count(r));
    } else {
      // We are about to change its class. Use a better name to remeber its
      // original source class on a collision. Tokenize the class name into
      // parts, and use them until no more collison.
      //
      // "com/facebook/foo/Bar;" => {"com", "facebook", "foo", "Bar"}
      std::string cls_name = show_deobfuscated(old_spec.cls);
      std::regex separator{"[/;]"};
      std::vector<std::string> parts;
      std::copy(std::sregex_token_iterator(cls_name.begin(), cls_name.end(),
                                           separator, -1),
                std::sregex_token_iterator(),
                std::back_inserter(parts));

      // Make a name like "name$Bar$foo", or "$clinit$$Bar$foo".
      std::stringstream ss;
      if (old_spec.name->str().front() == '<') {
        ss << "$"
           << old_spec.name->str().substr(1, old_spec.name->str().length() - 2)
           << "$";
      } else {
        ss << *old_spec.name;
      }
      for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
        ss << "$" << *part;
        r.name = DexString::make_string(ss.str());
        if (!s_method_map.count(r)) {
          break;
        }
      }
      // By this time, it should be no collision anymore.
    }
  }

  if (s_method_map.count(r)) {
    auto& m = *s_method_map.find(r)->second;
    always_assert_log(!s_method_map.count(r),
                      "Another method of the same signature already exists %s"
                      " %s %s",
                      SHOW(r.cls), SHOW(r.name), SHOW(r.proto));
  }
  s_method_map.emplace(r, method);

  // We just updated DexMethodSpec, which will update this method's name.
  // But we also need to update deobfuscated names properly, except for the
  // cases of ObfuscatePass. Otherwise, there won't be no 1:1 mapping between
  // obfuscated and deobfuscated names. See D13025081 for more detailed example.
  if (method->is_def() && update_deobfuscated_name) {
    // 'show(method)' correctly populates the name as 'show' dynamically builds
    // the name from its DexMethodSpec. We can safely use here.
    static_cast<DexMethod*>(method)->set_deobfuscated_name(show(method));
  }
}

void RedexContext::publish_class(DexClass* cls) {
  std::lock_guard<std::mutex> l(m_type_system_mutex);
  const DexType* type = cls->get_type();
  if (m_type_to_class.find(type) != end(m_type_to_class)) {
    const auto& prev_loc = m_type_to_class[type]->get_location();
    const auto& cur_loc = cls->get_location();
    if (prev_loc == cur_loc) {
      TRACE(MAIN, 1, "Warning: found a duplicate class: %s\n", SHOW(cls));
    } else {
      std::string class_name = show(cls);
      std::string dex_1 = m_type_to_class[type]->get_location();
      std::string dex_2 = cls->get_location();

      TRACE(MAIN,
            1,
            "ABORT! Found a duplicate class: %s in two dexes:\ndex 1: %s\ndex "
            "2: %s\n",
            class_name.c_str(),
            dex_1.c_str(),
            dex_2.c_str());

      throw duplicate_class(class_name, dex_1, dex_2);
    }
  }
  m_type_to_class.emplace(type, cls);
}

DexClass* RedexContext::type_class(const DexType* t) {
  auto it = m_type_to_class.find(t);
  return it != m_type_to_class.end() ? it->second : nullptr;
}
