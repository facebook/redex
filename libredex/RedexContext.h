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
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "DexMemberRefs.h"

class DexDebugInstruction;
class DexString;
class DexType;
class DexFieldRef;
class DexTypeList;
class DexProto;
class DexMethodRef;
class DexClass;
struct DexFieldSpec;
struct DexDebugEntry;
struct DexPosition;
struct RedexContext;

extern RedexContext* g_redex;

struct RedexContext {
  RedexContext();
  ~RedexContext();

  DexString* make_string(const char* nstr, uint32_t utfsize);
  DexString* get_string(const char* nstr, uint32_t utfsize);

  DexType* make_type(DexString* dstring);
  DexType* get_type(DexString* dstring);
  void alias_type_name(DexType* type, DexString* new_name);

  DexFieldRef* make_field(const DexType* container,
                          const DexString* name,
                          const DexType* type);
  DexFieldRef* get_field(const DexType* container,
                         const DexString* name,
                         const DexType* type);

  void erase_field(DexFieldRef*);
  void mutate_field(DexFieldRef* field,
                    const DexFieldSpec& ref,
                    bool rename_on_collision = false);

  DexTypeList* make_type_list(std::deque<DexType*>&& p);
  DexTypeList* get_type_list(std::deque<DexType*>&& p);

  DexProto* make_proto(DexType* rtype,
                       DexTypeList* args,
                       DexString* shorty);
  DexProto* get_proto(DexType* rtype, DexTypeList* args);

  DexMethodRef* make_method(DexType* type,
                         DexString* name,
                         DexProto* proto);
  DexMethodRef* get_method(DexType* type,
                        DexString* name,
                        DexProto* proto);
  void erase_method(DexMethodRef*);
  void mutate_method(DexMethodRef* method,
                     const DexMethodSpec& ref,
                     bool rename_on_collision = false);

  DexDebugEntry* make_dbg_entry(DexDebugInstruction* opcode);
  DexDebugEntry* make_dbg_entry(DexPosition* pos);

  void publish_class(DexClass*);
  DexClass* type_class(const DexType* t);
  template <class TypeClassWalkerFn = void(const DexType*, const DexClass*)>
  void walk_type_class(TypeClassWalkerFn walker) {
    for (const auto& type_cls : m_type_to_class) {
      walker(type_cls.first, type_cls.second);
    }
  }

  /*
   * This returns true if we want to enable features that will only go out
   * in the next quarterly release.
   */
  static bool next_release_gate() { return g_redex->m_next_release_gate; }
  static void set_next_release_gate(bool v) {
    g_redex->m_next_release_gate = v;
  }

 private:
  struct carray_cmp {
    bool operator()(const char* a, const char* b) const {
      return (strcmp(a, b) < 0);
    }
  };

  // DexString
  std::map<const char*, DexString*, carray_cmp> s_string_map;
  std::mutex s_string_lock;

  // DexType
  std::unordered_map<DexString*, DexType*> s_type_map;
  std::mutex s_type_lock;

  // DexFieldRef
  std::unordered_map<DexFieldSpec, DexFieldRef*> s_field_map;
  std::mutex s_field_lock;

  // DexTypeList
  std::map<std::deque<DexType*>, DexTypeList*> s_typelist_map;
  std::mutex s_typelist_lock;

  // DexProto
  std::unordered_map<DexType*, std::unordered_map<DexTypeList*, DexProto*>>
      s_proto_map;
  std::mutex s_proto_lock;

  // DexMethod
  std::unordered_map<DexMethodSpec, DexMethodRef*> s_method_map;
  std::mutex s_method_lock;

  // Type-to-class map and class hierarchy
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;

  const std::vector<const DexType*> m_empty_types;

  bool m_next_release_gate{false};
};

class malformed_dex : public std::exception {
 public:
  malformed_dex(const std::string& class_name,
                const std::string& dex_1,
                const std::string& dex_2)
      : m_class_name(class_name),
        m_dex_1(dex_1),
        m_dex_2(dex_2),
        m_msg(make_msg(class_name, dex_1, dex_2)) {}

  virtual const char* what() const throw() { return m_msg.c_str(); }

  const std::string m_class_name;
  const std::string m_dex_1;
  const std::string m_dex_2;

 private:
  const std::string m_msg;

  std::string make_msg(const std::string& class_name,
                       const std::string& dex_1,
                       const std::string& dex_2) {
    std::ostringstream oss;
    oss << "Found duplicate class in two different dex files. Class "
        << m_class_name;

    return oss.str();
  }
};

// One or more exceptions
class aggregate_exception : public std::exception {
 public:
  explicit aggregate_exception(const std::vector<std::exception_ptr>& exns)
      : m_exceptions(exns) {}

  // We do not really want to have this called directly
  virtual const char* what() const throw() { return "one or more exception"; }

  const std::vector<std::exception_ptr> m_exceptions;
};
