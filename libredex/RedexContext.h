/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <boost/functional/hash.hpp>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "ConcurrentContainers.h"
#include "DexMemberRefs.h"
#include "KeepReason.h"

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

  /**
   * Change the name of a type, but do not remove the old name from the mapping
   */
  void set_type_name(DexType* type, DexString* new_name);
  /**
   * Add an additional name to refer to a type (a deobfuscated name for example)
   */
  void alias_type_name(DexType* type, DexString* new_name);
  /**
   * Remove a name -> type entry from the map
   */
  void remove_type_name(DexString* name);

  DexFieldRef* make_field(const DexType* container,
                          const DexString* name,
                          const DexType* type);
  DexFieldRef* get_field(const DexType* container,
                         const DexString* name,
                         const DexType* type);

  void erase_field(DexFieldRef*);
  void mutate_field(DexFieldRef* field,
                    const DexFieldSpec& ref,
                    bool rename_on_collision,
                    bool update_deobfuscated_name);

  DexTypeList* make_type_list(std::deque<DexType*>&& p);
  DexTypeList* get_type_list(std::deque<DexType*>&& p);

  DexProto* make_proto(DexType* rtype, DexTypeList* args, DexString* shorty);
  DexProto* get_proto(DexType* rtype, DexTypeList* args);

  DexMethodRef* make_method(DexType* type, DexString* name, DexProto* proto);
  DexMethodRef* get_method(DexType* type, DexString* name, DexProto* proto);

  void erase_method(DexMethodRef*);
  void mutate_method(DexMethodRef* method,
                     const DexMethodSpec& new_spec,
                     bool rename_on_collision,
                     bool update_deobfuscated_name);

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
   * This returns true if we want to preserve keep reasons for better
   * diagnostics.
   */
  static bool record_keep_reasons() { return g_redex->m_record_keep_reasons; }
  static void set_record_keep_reasons(bool v) {
    g_redex->m_record_keep_reasons = v;
  }

  template <class... Args>
  static keep_reason::Reason* make_keep_reason(Args&&... args) {
    auto to_insert =
        std::make_unique<keep_reason::Reason>(std::forward<Args>(args)...);
    if (g_redex->s_keep_reasons.emplace(to_insert.get(), to_insert.get())) {
      return to_insert.release();
    }
    return g_redex->s_keep_reasons.at(to_insert.get());
  }

 private:
  struct Strcmp;
  struct TruncatedStringHash;

  // We use this instead of an unordered_map-based ConcurrentMap because
  // hashing is expensive on large strings -- it has to hash the entire key to
  // find its bucket. An std::map (i.e. a tree) performs better with large keys
  // in a sparse keyset because it only needs to find the first character that
  // differs between keys when traversing the tree, meaning that it usually
  // doesn't need to examine the entire key for insertions.
  //
  // We still need to do hashing in order to shard the keys across the
  // individually-locked std::maps, but it suffices to hash a substring for this
  // purpose.
  template <typename Value, size_t n_slots = 31>
  using ConcurrentLargeStringMap =
      ConcurrentMapContainer<std::map<const char*, Value, Strcmp>,
                             const char*,
                             Value,
                             TruncatedStringHash,
                             n_slots>;

  struct Strcmp {
    bool operator()(const char* a, const char* b) const {
      return strcmp(a, b) < 0;
    }
  };

  // Hash an 8-byte subsequence of a given string, offset by 32 bytes from the
  // start. Dex files tend to contain many strings with the same prefixes,
  // because every class / method under a given package will share the same
  // prefix. The offset ensures that we have more unique subsequences to hash.
  //
  // XXX(jezng): 32 was picked fairly arbitrarily; testing showed that it was
  // definitely better than 0, but I did not test any numbers in between.
  struct TruncatedStringHash {
    size_t operator()(const char* s) {
      constexpr size_t hash_prefix_len = 8;
      constexpr size_t offset = 32;
      size_t len = strnlen(s, offset + hash_prefix_len);
      size_t start = std::max<int64_t>(0, int64_t(len - hash_prefix_len));
      return boost::hash_range(s + start, s + len);
    }
  };

  // DexString
  ConcurrentLargeStringMap<DexString*> s_string_map;

  // DexType
  ConcurrentMap<DexString*, DexType*> s_type_map;

  // DexFieldRef
  ConcurrentMap<DexFieldSpec, DexFieldRef*> s_field_map;
  std::mutex s_field_lock;

  // DexTypeList
  ConcurrentMap<std::deque<DexType*>,
                DexTypeList*,
                boost::hash<std::deque<DexType*>>>
      s_typelist_map;

  // DexProto
  using ProtoKey = std::pair<DexType*, DexTypeList*>;
  ConcurrentMap<ProtoKey, DexProto*, boost::hash<ProtoKey>> s_proto_map;

  // DexMethod
  ConcurrentMap<DexMethodSpec, DexMethodRef*> s_method_map;
  std::mutex s_method_lock;

  // Type-to-class map and class hierarchy
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;

  const std::vector<const DexType*> m_empty_types;

  ConcurrentMap<keep_reason::Reason*,
                keep_reason::Reason*,
                keep_reason::ReasonPtrHash,
                keep_reason::ReasonPtrEqual>
      s_keep_reasons;

  bool m_record_keep_reasons{false};
};

class duplicate_class : public std::exception {
 public:
  duplicate_class(const std::string& class_name,
                  const std::string& location_1,
                  const std::string& location_2)
      : m_class_name(class_name),
        m_location_1(location_1),
        m_location_2(location_2),
        m_msg(make_msg(class_name, location_1, location_2)) {}

  const char* what() const throw() override { return m_msg.c_str(); }

  const std::string m_class_name;
  const std::string m_location_1;
  const std::string m_location_2;

 private:
  const std::string m_msg;

  std::string make_msg(const std::string& class_name,
                       const std::string& location_1,
                       const std::string& location_2) {
    std::ostringstream oss;
    oss << "Found duplicate class in two different files. Class "
        << m_class_name << "\n"
        << "  1: " << location_1 << "\n"
        << "  2: " << location_2 << "\n";

    return oss.str();
  }
};

// One or more exceptions
class aggregate_exception : public std::exception {
 public:
  explicit aggregate_exception(const std::vector<std::exception_ptr>& exns)
      : m_exceptions(exns) {}

  // We do not really want to have this called directly
  const char* what() const throw() override { return "one or more exception"; }

  const std::vector<std::exception_ptr> m_exceptions;
};
