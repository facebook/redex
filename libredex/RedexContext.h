/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <atomic>
#include <boost/functional/hash.hpp>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ConcurrentContainers.h"
#include "Debug.h"
#include "DexMemberRefs.h"
#include "FrequentlyUsedPointersCache.h"

class DexCallSite;
class DexClass;
class DexLocation;
class DexDebugInstruction;
class DexField;
class DexFieldRef;
class DexMethod;
class DexMethodHandle;
class DexMethodRef;
class DexProto;
class DexString;
class DexType;
class DexTypeList;
class PositionPatternSwitchManager;
struct DexDebugEntry;
struct DexFieldSpec;
struct DexPosition;
struct RedexContext;
namespace keep_rules {
struct AssumeReturnValue;
} // namespace keep_rules

extern RedexContext* g_redex;

#if defined(__SSE4_2__) && defined(__linux__) && defined(__STRCMP_LESS__)
extern "C" bool strcmp_less(const char* str1, const char* str2);
#endif

struct RedexContext {
  explicit RedexContext(bool allow_class_duplicates = false);
  ~RedexContext();

  const DexString* make_string(std::string_view s);
  const DexString* get_string(std::string_view s);

  DexType* make_type(const DexString* dstring);
  DexType* get_type(const DexString* dstring);

  /**
   * Change the name of a type, but do not remove the old name from the mapping
   */
  void set_type_name(DexType* type, const DexString* new_name);
  /**
   * Add an additional name to refer to a type (a deobfuscated name for example)
   */
  void alias_type_name(DexType* type, const DexString* new_name);
  /**
   * Remove a name -> type entry from the map
   */
  void remove_type_name(const DexString* name);

  DexFieldRef* make_field(const DexType* container,
                          const DexString* name,
                          const DexType* type);
  DexFieldRef* get_field(const DexType* container,
                         const DexString* name,
                         const DexType* type);

  /**
   * Add an additional name to refer to a field (a deobfuscated name for
   * example)
   */
  void alias_field_name(DexFieldRef* field, const DexString* new_name);

  void erase_field(DexFieldRef*);
  void erase_field(const DexType* container,
                   const DexString* name,
                   const DexType* type);
  void mutate_field(DexFieldRef* field,
                    const DexFieldSpec& ref,
                    bool rename_on_collision);

  using DexTypeListContainerType = std::vector<DexType*>;

  DexTypeList* make_type_list(DexTypeListContainerType&& p);
  DexTypeList* get_type_list(const DexTypeListContainerType& p);

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

  /**
   * Add an additional name to refer to a method (a deobfuscated name for
   * example)
   */
  void alias_method_name(DexMethodRef* method, const DexString* new_name);

  DexMethodHandle* make_methodhandle();
  DexMethodHandle* get_methodhandle();

  void erase_method(DexMethodRef*);
  void erase_method(const DexType* type,
                    const DexString* name,
                    const DexProto* proto);
  void mutate_method(DexMethodRef* method,
                     const DexMethodSpec& new_spec,
                     bool rename_on_collision);

  DexLocation* make_location(std::string_view store_name,
                             std::string_view file_name);
  DexLocation* get_location(std::string_view store_name,
                            std::string_view file_name);

  PositionPatternSwitchManager* get_position_pattern_switch_manager();

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

  // Add a lambda to be called when RedexContext is destructed. This is
  // especially useful for resetting caches/singletons in tests.
  using Task = std::function<void(void)>;
  void add_destruction_task(const Task& t);

  static constexpr bool kDebugPointersCacheLoad = false;
  void load_pointers_cache() {
    m_pointers_cache.load();
    m_pointers_cache_loaded = true;
  }
  const FrequentlyUsedPointers& pointers_cache() {
    if (!m_pointers_cache_loaded) {
      redex_assert(!kDebugPointersCacheLoad);
      std::lock_guard<std::mutex> lock(m_pointers_cache_lock);
      load_pointers_cache();
    }
    return m_pointers_cache;
  }

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

  size_t num_sb_interaction_indices() const {
    return m_sb_interaction_indices.size();
  }
  size_t get_sb_interaction_index(const std::string& interaction) const {
    auto it = m_sb_interaction_indices.find(interaction);
    if (it == m_sb_interaction_indices.end()) {
      return std::numeric_limits<size_t>::max();
    }
    return it->second;
  }
  const std::unordered_map<std::string, size_t>& get_sb_interaction_indices()
      const {
    return m_sb_interaction_indices;
  }
  void set_sb_interaction_index(
      const std::unordered_map<std::string, size_t>& input);

  // This is for convenience.
  bool instrument_mode{false};

 private:
  struct Strcmp;
  struct TruncatedStringHash;

  // A thread-safe container for raw string storage
  struct ConcurrentStringStorage {
    static constexpr size_t n_slots = 11;
    // A not thread-safe container, holding individually allocated buffers
    struct Container {
      struct Buffer {
        const size_t allocated;
        size_t used{0};
        size_t remaining() const { return allocated - used; }
        const std::unique_ptr<char[]> chars;
        const Buffer* next;
        Buffer(size_t size, Buffer* next)
            : allocated(size),
              chars(std::make_unique<char[]>(size)),
              next(next) {}
      };
      // Default size for buffers, or 0 to create one perfectly sized buffer per
      // allocation
      const size_t default_size;
      Buffer* buffer{nullptr};
      explicit Container(size_t default_size) : default_size(default_size) {}
      ~Container();
      char* allocate(size_t length);
    };
    // A context for a temporarily acquired container that will be released to
    // its owner when the context is destructed
    struct Context {
      ConcurrentStringStorage* owner;
      size_t index;
      Container* container;
      ~Context();
    };
    struct Stats {
      size_t allocated{0};
      size_t used{0};
      size_t containers{0};
      size_t buffers{0};
      size_t waited{0};
      size_t contention{0};
      size_t sorted{0};
    };
    const size_t default_buffer_size;
    // Largest allowed individual allocation, or 0 to create arbitrarily
    // perfectly sized buffers
    const size_t max_allocation;
    // How many containers can be active concurrently
    const size_t max_containers;
    std::atomic<size_t> created{0};
    std::atomic<size_t> waited{0};
    std::atomic<size_t> contention{0};
    std::atomic<size_t> sorted{0};
    struct Slot {
      std::atomic<Container*> container{nullptr};
      uint8_t padding[64 - sizeof(std::atomic<Container*>)];
    };
    std::array<Slot, n_slots> slots;
    std::mutex pool_lock;
    std::vector<std::unique_ptr<Container>> pool;
    ConcurrentStringStorage(size_t default_buffer_size,
                            size_t max_allocation,
                            size_t max_containers)
        : default_buffer_size(default_buffer_size),
          max_allocation(max_allocation),
          max_containers(std::max(max_containers, n_slots)) {}
    Context get_context();
    Stats get_stats() const;
    ~ConcurrentStringStorage() {
      for (auto& slot : slots) {
        delete slot.container.load();
      }
    }
  };

  // Hashing is expensive on large strings (long Java type names, string
  // literals), so we avoid using `std::unordered_map` directly.
  //
  // For leaf-level storage we use `std::set` (i.e., a tree). In a sparse
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
  // this (see `ConcurrentProjectedStringSet`).
  //
  // The two layers give infrastructure overhead, however, the base size
  // of a `std::set` and `ConcurrentContainer` is quite small.
  //
  // We use `const DexString*` for the keys, however, we have to be careful not
  // to assume that the referenced `const char*` data is zero-terminated.
  using StringSetKey = const DexString*;
  struct StringSetKeyHash {
    size_t operator()(StringSetKey k) const;
  };
  struct StringSetKeyCompare {
    bool operator()(StringSetKey a, StringSetKey b) const;
  };

  template <size_t n_slots = 31>
  using ConcurrentProjectedStringSet = InsertOnlyConcurrentSetContainer<
      std::set<StringSetKey, StringSetKeyCompare>,
      StringSetKey,
      StringSetKeyHash,
      n_slots>;

  template <size_t n_slots, size_t m_slots>
  struct LargeStringSet {
    using AType = std::array<ConcurrentProjectedStringSet<n_slots>, m_slots>;

    AType sets;

    ConcurrentProjectedStringSet<n_slots>& at(StringSetKey k) {
      size_t hashed = TruncatedStringHash()(k) % m_slots;
      return sets[hashed];
    }

    typename AType::iterator begin() { return sets.begin(); }
    typename AType::iterator end() { return sets.end(); }
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
    size_t operator()(StringSetKey k);
  };

  // DexString
  LargeStringSet<31, 127> s_string_set;

  // We maintain three kinds of raw string storage
  ConcurrentStringStorage s_small_string_storage;
  ConcurrentStringStorage s_medium_string_storage;
  ConcurrentStringStorage s_large_string_storage;

  // DexType
  ConcurrentMap<const DexString*, DexType*> s_type_map;

  // DexFieldRef
  ConcurrentMap<DexFieldSpec, DexFieldRef*> s_field_map;
  std::mutex s_field_lock;

  // DexTypeList
  struct DexTypeListContainerTypePtrHash {
    size_t operator()(const DexTypeListContainerType* d) const {
      return boost::hash<DexTypeListContainerType>()(*d);
    }
  };
  struct DexTypeListContainerTypePtrEquals {
    size_t operator()(const DexTypeListContainerType* lhs,
                      const DexTypeListContainerType* rhs) const {
      return lhs == rhs || *lhs == *rhs;
    }
  };
  ConcurrentMap<const DexTypeListContainerType*,
                DexTypeList*,
                DexTypeListContainerTypePtrHash,
                DexTypeListContainerTypePtrEquals>
      s_typelist_map;

  // DexProto
  using ProtoKey = std::pair<const DexType*, const DexTypeList*>;
  ConcurrentMap<ProtoKey, DexProto*, boost::hash<ProtoKey>> s_proto_map;

  // DexMethod
  ConcurrentMap<DexMethodSpec, DexMethodRef*> s_method_map;
  std::mutex s_method_lock;

  // DexLocation
  using ClassLocationKey = std::pair<std::string_view, std::string_view>;
  struct ClassLocationKeyHash {
    size_t operator()(const ClassLocationKey& k) const {
      return std::hash<std::string_view>()(k.second);
    }
  };
  ConcurrentMap<ClassLocationKey, DexLocation*, ClassLocationKeyHash>
      s_location_map;

  // DexPositionSwitch and DexPositionPattern
  PositionPatternSwitchManager* m_position_pattern_switch_manager{nullptr};

  // Type-to-class map
  std::mutex m_type_system_mutex;
  std::unordered_map<const DexType*, DexClass*> m_type_to_class;
  std::vector<DexClass*> m_external_classes;

  const std::vector<const DexType*> m_empty_types;

  // These functions will be called when ~RedexContext() is called
  std::mutex m_destruction_tasks_lock;
  std::vector<Task> m_destruction_tasks;

  std::unordered_map<std::string, size_t> m_sb_interaction_indices;

  bool m_allow_class_duplicates;

  bool m_pointers_cache_loaded{false};
  std::mutex m_pointers_cache_lock;
  FrequentlyUsedPointers m_pointers_cache;

  // Field values map specified by Proguard assume value
  ConcurrentMap<DexField*, std::unique_ptr<keep_rules::AssumeReturnValue>>
      field_values;
  // Return values map specified by Proguard assume value
  ConcurrentMap<DexMethod*, std::unique_ptr<keep_rules::AssumeReturnValue>>
      method_return_values;
};
