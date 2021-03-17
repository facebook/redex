/*
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
#include "FrequentlyUsedPointersCache.h"
#include "KeepReason.h"

class DexCallSite;
class DexDebugInstruction;
class DexString;
class DexType;
class DexFieldRef;
class DexTypeList;
class DexProto;
class DexMethodRef;
class DexMethodHandle;
class DexClass;
class DexField;
struct DexFieldSpec;
struct DexDebugEntry;
struct DexPosition;
struct RedexContext;
namespace keep_rules {
struct AssumeReturnValue;
}

extern RedexContext* g_redex;

#if defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
extern "C" bool strcmp_less(const char* str1, const char* str2);
#endif

struct RedexContext {
  explicit RedexContext(bool allow_class_duplicates = false);
  ~RedexContext();

  DexString* make_string(const char* nstr, uint32_t utfsize);
  DexString* get_string(const char* nstr, uint32_t utfsize);

  DexType* make_type(const DexString* dstring);
  DexType* get_type(const DexString* dstring);

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
                    bool rename_on_collision);

  DexTypeList* make_type_list(std::deque<DexType*>&& p);
  DexTypeList* get_type_list(std::deque<DexType*>&& p);

  DexProto* make_proto(const DexType* rtype,
                       const DexTypeList* args,
                       const DexString* shorty);
  DexProto* get_proto(const DexType* rtype, const DexTypeList* args);

  DexMethodRef* make_method(const DexType* type,
                            const DexString* name,
                            const DexProto* proto);
  DexMethodRef* get_method(const DexType* type,
                           const DexString* name,
                           const DexProto* proto);

  DexMethodHandle* make_methodhandle();
  DexMethodHandle* get_methodhandle();

  void erase_method(DexMethodRef*);
  void mutate_method(DexMethodRef* method,
                     const DexMethodSpec& new_spec,
                     bool rename_on_collision);

  DexDebugEntry* make_dbg_entry(DexDebugInstruction* opcode);
  DexDebugEntry* make_dbg_entry(DexPosition* pos);

  // Return false on unique classes
  // Return true on benign duplicate classes
  // Throw RedexException on problematic duplicate classes
  bool class_already_loaded(DexClass* cls);

  void publish_class(DexClass* cls);

  DexClass* type_class(const DexType* t);
  template <class TypeClassWalkerFn = void(const DexType*, const DexClass*)>
  void walk_type_class(TypeClassWalkerFn walker) {
    for (const auto& type_cls : m_type_to_class) {
      walker(type_cls.first, type_cls.second);
    }
  }

  const std::vector<DexClass*>& external_classes() const {
    return m_external_classes;
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

  // Add a lambda to be called when RedexContext is destructed. This is
  // especially useful for resetting caches/singletons in tests.
  using Task = std::function<void(void)>;
  void add_destruction_task(const Task& t) { m_destruction_tasks.push_back(t); }

  FrequentlyUsedPointers pointers_cache() { return m_pointers_cache; }

  // Set and return field values keep_rules::AssumeReturnValue provided by
  // proguard rules.
  void set_field_value(DexField* field, keep_rules::AssumeReturnValue& val);
  keep_rules::AssumeReturnValue* get_field_value(DexField* field);
  void unset_field_value(DexField* field);

  // Set and return method's keep_rules::AssumeReturnValue provided by proguard
  // rules.
  void set_return_value(DexMethod* method, keep_rules::AssumeReturnValue& val);
  keep_rules::AssumeReturnValue* get_return_value(DexMethod* method);
  void unset_return_value(DexMethod* method);

 private:
  struct Strcmp;
  struct TruncatedStringHash;

  // Hashing is expensive on large strings (long Java type names, string
  // literals), so we avoid using `std::unordered_map` directly.
  //
  // For leaf-level storage we use `std::map` (i.e., a tree). In a sparse
  // string keyset with large keys this performs better as only the suffix
  // until first change needs to be compared.
  //
  // For sharding, we use two layers. The first layer is a partial string
  // hash as defined by `TruncatedStringHash`. It picks a segment "close"
  // to the front and performs reasonably well. A std::array is used for
  // sharding here (see `LargeStringMap`).
  //
  // The second layer optimizes the string comparison. We have additional
  // data besides the string data pointer, namely the UTF size. We can
  // avoid comparisons for different string lengths. The second layer
  // thus shards over it. We use the `ConcurrentContainer` sharding for
  // this (see `ConcurrentProjectedStringMap`).
  //
  // The two layers give infrastructure overhead, however, the base size
  // of a `std::map` and `ConcurrentContainer` is quite small.

  using StringMapKey = std::pair<const char*, uint32_t>;
  struct StringMapKeyHash {
    size_t operator()(const StringMapKey& k) const { return k.second; }
  };
  struct StringMapKeyProjection {
    const char* operator()(const StringMapKey& k) const { return k.first; }
  };

  template <size_t n_slots = 31>
  using ConcurrentProjectedStringMap =
      ConcurrentMapContainer<std::map<const char*, DexString*, Strcmp>,
                             StringMapKey,
                             DexString*,
                             StringMapKeyHash,
                             StringMapKeyProjection,
                             n_slots>;

  template <size_t n_slots, size_t m_slots>
  struct LargeStringMap {
    using AType = std::array<ConcurrentProjectedStringMap<n_slots>, m_slots>;

    AType map;

    ConcurrentProjectedStringMap<n_slots>& at(const StringMapKey& k) {
      size_t hashed = TruncatedStringHash()(k.first) % m_slots;
      return map[hashed];
    }

    typename AType::iterator begin() { return map.begin(); }
    typename AType::iterator end() { return map.end(); }
  };

  struct Strcmp {
    bool operator()(const char* a, const char* b) const {
#if defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
      return strcmp_less(a, b);
#else
      return strcmp(a, b) < 0;
#endif
    }
  };

  // Hash a 32-byte subsequence of a given string, offset by 32 bytes from the
  // start. Dex files tend to contain many strings with the same prefixes,
  // because every class / method under a given package will share the same
  // prefix. The offset ensures that we have more unique subsequences to hash.
  //
  // An offset of 32 and hash prefix length of 32 seemed to perform best on the
  // typical strings in an android app. It's important to remain within one
  // cache line (offset + hash_prefix_len <= 64) and hash enough of the string
  // to minimize the chance of duplicate sections
  struct TruncatedStringHash {
    size_t operator()(const char* s) {
      constexpr size_t hash_prefix_len = 32;
      constexpr size_t offset = 32;
      size_t len = strnlen(s, offset + hash_prefix_len);
      size_t start = std::max<int64_t>(0, int64_t(len - hash_prefix_len));
      return boost::hash_range(s + start, s + len);
    }
  };

  // DexString
  LargeStringMap<31, 127> s_string_map;

  // DexType
  ConcurrentMap<const DexString*, DexType*> s_type_map;

  // DexFieldRef
  ConcurrentMap<DexFieldSpec, DexFieldRef*> s_field_map;
  std::mutex s_field_lock;

  // DexTypeList
  ConcurrentMap<std::deque<DexType*>,
                DexTypeList*,
                boost::hash<std::deque<DexType*>>>
      s_typelist_map;

  // DexProto
  using ProtoKey = std::pair<const DexType*, const DexTypeList*>;
  ConcurrentMap<ProtoKey, DexProto*, boost::hash<ProtoKey>> s_proto_map;

  // DexMethod
  ConcurrentMap<DexMethodSpec, DexMethodRef*> s_method_map;
  std::mutex s_method_lock;

  // Type-to-class map
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;
  std::vector<DexClass*> m_external_classes;

  const std::vector<const DexType*> m_empty_types;

  ConcurrentMap<keep_reason::Reason*,
                keep_reason::Reason*,
                keep_reason::ReasonPtrHash,
                keep_reason::ReasonPtrEqual>
      s_keep_reasons;

  // These functions will be called when ~RedexContext() is called
  std::vector<Task> m_destruction_tasks;

  bool m_record_keep_reasons{false};
  bool m_allow_class_duplicates;

  FrequentlyUsedPointers m_pointers_cache;

  // Field values map specified by Proguard assume value
  ConcurrentMap<DexField*, std::unique_ptr<keep_rules::AssumeReturnValue>>
      field_values;
  // Return values map specified by Proguard assume value
  ConcurrentMap<DexMethod*, std::unique_ptr<keep_rules::AssumeReturnValue>>
      method_return_values;
};

// One or more exceptions
class aggregate_exception : public std::exception {
 public:
  template <class T>
  explicit aggregate_exception(T container)
      : m_exceptions(container.begin(), container.end()) {}

  // We do not really want to have this called directly
  const char* what() const throw() override { return "one or more exception"; }

  const std::vector<std::exception_ptr> m_exceptions;
};
void run_rethrow_first_aggregate(const std::function<void()>& f);
