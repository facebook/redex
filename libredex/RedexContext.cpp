/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexContext.h"

#include <boost/thread/thread.hpp>
#include <exception>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "Debug.h"
#include "DexCallSite.h"
#include "DexClass.h"
#include "DexPosition.h"
#include "DuplicateClasses.h"
#include "KeepReason.h"
#include "ProguardConfiguration.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "WorkQueue.h"

static_assert(std::is_same<DexTypeList::ContainerType,
                           RedexContext::DexTypeListContainerType>::value);

RedexContext* g_redex;

RedexContext::RedexContext(bool allow_class_duplicates)
    : s_small_string_storage{16384, 111,
                             boost::thread::hardware_concurrency() / 2},
      s_medium_string_storage{65536, 2000,
                              boost::thread::hardware_concurrency() / 4},
      s_large_string_storage{0, 0, boost::thread::hardware_concurrency()},
      m_allow_class_duplicates(allow_class_duplicates) {}

RedexContext::~RedexContext() {
  std::vector<std::function<void()>> fns{
      [&] {
        Timer timer("Delete DexTypes", /* indent */ false);
        // NB: This table intentionally contains aliases (multiple
        // DexStrings map to the same DexType), so we have to dedup the set of
        // types before deleting to avoid double-frees.
        std::unordered_set<DexType*> delete_types;
        for (auto const& p : s_type_map) {
          if (delete_types.emplace(p.second).second) {
            delete p.second;
          }
        }
        s_type_map.clear();
      },
      [&] {
        Timer timer("DexTypeLists", /* indent */ false);
        for (auto const& p : s_typelist_map) {
          delete p.second;
        }
        s_typelist_map.clear();
      },
      [&] {
        Timer timer("Delete DexProtos.", /* indent */ false);
        for (auto* proto : s_proto_set) {
          delete proto;
        }
        s_proto_set.clear();
      },
      [&] {
        Timer timer("Delete DexClasses", /* indent */ false);
        for (auto const& p : m_type_to_class) {
          delete p.second;
        }
        m_type_to_class.clear();
      },
      [&] {
        Timer timer("Delete DexLocations", /* indent */ false);
        for (auto const& p : s_location_map) {
          delete p.second;
        }
        s_location_map.clear();
      },
      [&] {
        Timer timer("release_keep_reasons", /* indent */ false);
        keep_reason::Reason::release_keep_reasons();
      },
      [&] {
        Timer timer("m_destruction_tasks", /* indent */ false);
        for (const Task& t : m_destruction_tasks) {
          t();
        }
        m_destruction_tasks.clear();
      },
      [&] {
        Timer timer("delete m_position_pattern_switch_manager",
                    /* indent */ false);
        delete m_position_pattern_switch_manager;
      },
      [&] {
        Timer timer("misc", /* indent */ false);
        m_external_classes.clear();
        field_values.clear();
        method_return_values.clear();
      }};
  // Deleting fields and methods is especially expensive, so we do it by
  // "buckets".
  const size_t method_buckets_count = 16;
  for (size_t bucket = 0; bucket < method_buckets_count; bucket++) {
    fns.push_back([bucket, this]() {
      Timer timer("Delete DexMethods/" + std::to_string(bucket),
                  /* indent */ false);
      // Delete DexMethods. Use set to prevent double freeing aliases
      std::unordered_set<DexMethod*> delete_methods;
      for (auto const& it : s_method_map) {
        auto method = static_cast<DexMethod*>(it.second);
        if ((reinterpret_cast<size_t>(method) >> 16) % method_buckets_count ==
                bucket &&
            delete_methods.emplace(method).second) {
          delete method;
        }
      }
    });
  }
  const size_t field_buckets_count = 4;
  for (size_t bucket = 0; bucket < field_buckets_count; bucket++) {
    fns.push_back([bucket, this]() {
      Timer timer("Delete DexFields/" + std::to_string(bucket),
                  /* indent */ false);
      // Delete DexFields. Use set to prevent double freeing aliases
      std::unordered_set<DexField*> delete_fields;
      for (auto const& it : s_field_map) {
        auto field = static_cast<DexField*>(it.second);
        if ((reinterpret_cast<size_t>(field) >> 16) % field_buckets_count ==
                bucket &&
            delete_fields.emplace(field).second) {
          delete field;
        }
      }
    });
  }
  size_t segment_index{0};
  for (auto& segment : s_string_set) {
    fns.push_back([&segment, index = segment_index++]() {
      Timer timer("Delete DexStrings segment/" + std::to_string(index),
                  /* indent */ false);
      for (auto* v : segment) {
        delete v;
      }
      segment.clear();
    });
  }

  workqueue_run<std::function<void()>>([](std::function<void()>& fn) { fn(); },
                                       fns);
  s_method_map.clear();

  std::ostringstream oss;
  auto log_stats = [&oss](auto* name, ConcurrentStringStorage& storage) {
    auto stats = storage.get_stats();
    oss << "\n  " << name << ": " << stats.containers << " containers with "
        << stats.buffers << " buffers, " << stats.used << " / "
        << stats.allocated << " bytes used / allocated ("
        << (stats.allocated == 0 ? 100 : (100 * stats.used / stats.allocated))
        << "%%), " << stats.waited << " / " << stats.contention
        << " times waited / contended, " << stats.sorted << " times sorted";
  };
  log_stats("small", s_small_string_storage);
  log_stats("medium", s_medium_string_storage);
  log_stats("large", s_large_string_storage);
  TRACE(PM, 1, "String storage @ %u hardware concurrency:%s",
        boost::thread::hardware_concurrency(), oss.str().c_str());
}

/*
 * Try and insert :key into :container. This insertion may fail if
 * another thread has already inserted that key. In that case, return the
 * existing value and discard the one we were trying to insert.
 */
template <class Key, class Deleter = std::default_delete<Key>, class Container>
static const Key* const* try_insert(std::unique_ptr<Key, Deleter> key,
                                    Container* container) {
  auto [rv_ptr, inserted] = container->insert(key.get());
  if (inserted) {
    (void)key.release();
  }
  return rv_ptr;
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
                               std::unique_ptr<InsertValue, Deleter> value,
                               Container* container) {
  if (container->emplace(key, value.get())) {
    return value.release();
  }
  return container->at(key);
}

RedexContext::ConcurrentStringStorage::Container::~Container() {
  for (const auto* p = buffer; p;) {
    auto next = p->next;
    delete p;
    p = next;
  }
}

char* RedexContext::ConcurrentStringStorage::Container::allocate(
    size_t length) {
  if (buffer == nullptr || buffer->used + length > buffer->allocated) {
    buffer = new Buffer(default_size == 0 ? length : default_size, buffer);
  }
  auto storage = buffer->chars.get() + buffer->used;
  buffer->used += length;
  return storage;
}

RedexContext::ConcurrentStringStorage::Context
RedexContext::ConcurrentStringStorage::get_context() {
#if !IS_WINDOWS
  static std::atomic<size_t> next_index = 0;
  thread_local size_t index_plus_1 = 0;
  if (index_plus_1 == 0) {
    index_plus_1 = (next_index++ % n_slots) + 1;
  }
  size_t index = index_plus_1 - 1;
#else
  size_t index = 0;
#endif

  while (true) {
    auto* string_storage = slots[index].container.exchange(nullptr);
    if (string_storage) {
      std::atomic_thread_fence(std::memory_order_acquire);
      return {this, index, string_storage};
    }
    contention++;
    std::lock_guard<std::mutex> mutex(pool_lock);
    if (!pool.empty()) {
      string_storage = pool.back().release();
      pool.pop_back();
      return {this, index, string_storage};
    }
    if (created.fetch_add(1) < max_containers) {
      contention--;
      // add one more
      return {this, index, new Container(default_buffer_size)};
    }
    created.fetch_sub(1);
    // we know we just have to wait, so spin until we get something
    waited++;
#if !IS_WINDOWS
    // Apparently we are fighting against some other thread; move on to next
    // slot to reduce fighting odds
    index_plus_1 = (next_index++ % n_slots) + 1;
    index = index_plus_1 - 1;
#else
    index = (index + 1) % n_slots;
#endif
  }
}

RedexContext::ConcurrentStringStorage::Stats
RedexContext::ConcurrentStringStorage::get_stats() const {
  Stats stats;
  stats.waited = waited.load();
  stats.contention = contention.load();
  stats.sorted = sorted.load();
  auto add = [&stats](auto* storage) {
    if (!storage) {
      return;
    }
    for (const auto* p = storage->buffer; p; p = p->next) {
      stats.allocated += p->allocated;
      stats.used += p->used;
      stats.buffers++;
    }
    stats.containers++;
  };
  for (auto& slot : slots) {
    add(slot.container.load());
  }
  for (auto& storage : pool) {
    add(storage.get());
  }
  return stats;
}

RedexContext::ConcurrentStringStorage::Context::~Context() {
  auto* other_container = owner->slots[index].container.exchange(container);
  if (other_container == nullptr) {
    std::atomic_thread_fence(std::memory_order_release);
    return;
  }
  std::lock_guard<std::mutex> mutex(owner->pool_lock);
  auto& owner_pool = owner->pool;
  owner_pool.emplace_back(other_container);
  if (other_container->buffer->remaining() < owner->max_allocation &&
      owner_pool.size() >= 2) {
    owner->sorted++;
    std::sort(owner_pool.begin(), owner_pool.end(), [](auto& a, auto& b) {
      return a->buffer->remaining() < b->buffer->remaining();
    });
  }
}

const DexString* RedexContext::make_string(std::string_view str) {
  // We are creating a DexString key that is just "defined enough" to be used as
  // a key into our string set. The provided string does not have to be zero
  // terminated, and we won't compute the utf size, as neither is needed for
  // this purpose.
  uint32_t dummy_utfsize{0};
  const DexString key(str.data(), str.size(), dummy_utfsize);
  auto& segment = s_string_set.at(&key);

  auto rv_ptr = segment.get(&key);
  if (rv_ptr != nullptr) {
    return *rv_ptr;
  }

  ConcurrentStringStorage& concurrent_string_storage =
      str.length() < s_small_string_storage.max_allocation
          ? s_small_string_storage
      : str.length() < s_medium_string_storage.max_allocation
          ? s_medium_string_storage
          : s_large_string_storage;

  char* storage;
  {
    auto storage_context = concurrent_string_storage.get_context();

    // Note that DexStrings are keyed by a string_view created from the actual
    // storage. The string_view is valid until the storage is destroyed.
    storage = storage_context.container->allocate(str.length() + 1);
  }
  memcpy(storage, str.data(), str.length());
  storage[str.length()] = 0;

  uint32_t utfsize = length_of_utf8_string(storage);
  std::unique_ptr<DexString> string(
      new DexString(storage, str.length(), utfsize));
  return *try_insert(std::move(string), &segment);
  // If unsuccessful, we have wasted a bit of string storage. Oh well...
}

size_t RedexContext::StringSetKeyHash::operator()(StringSetKey k) const {
  return k->size();
}

bool RedexContext::StringSetKeyCompare::operator()(StringSetKey a,
                                                   StringSetKey b) const {
  if (a->size() != b->size()) {
    return a->size() < b->size();
  }
  return memcmp(a->c_str(), b->c_str(), a->size()) < 0;
}

size_t RedexContext::TruncatedStringHash::operator()(StringSetKey k) {
  const char* s = k->c_str();
  uint32_t string_size = k->size();
  constexpr size_t hash_prefix_len = 32;
  constexpr size_t offset = 32;
  size_t len = std::min<size_t>(string_size, offset + hash_prefix_len);
  size_t start = std::max<int64_t>(0, int64_t(len - hash_prefix_len));
  return boost::hash_range(s + start, s + len);
}

const DexString* RedexContext::get_string(std::string_view str) {
  uint32_t dummy_utfsize{0};
  const DexString key(str.data(), str.size(), dummy_utfsize);
  const auto& segment = s_string_set.at(&key);
  auto rv_ptr = segment.get(&key);
  return rv_ptr == nullptr ? nullptr : *rv_ptr;
}

DexType* RedexContext::make_type(const DexString* dstring) {
  always_assert(dstring != nullptr);
  auto rv = s_type_map.get(dstring, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  std::unique_ptr<DexType> type(new DexType(dstring));
  return try_insert(dstring, std::move(type), &s_type_map);
}

DexType* RedexContext::get_type(const DexString* dstring) {
  if (dstring == nullptr) {
    return nullptr;
  }
  return s_type_map.get(dstring, nullptr);
}

void RedexContext::set_type_name(DexType* type, const DexString* new_name) {
  alias_type_name(type, new_name);
  type->m_name = new_name;
}

void RedexContext::alias_type_name(DexType* type, const DexString* new_name) {
  always_assert_log(
      !s_type_map.count(new_name),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  s_type_map.emplace(new_name, type);
}

void RedexContext::remove_type_name(const DexString* name) {
  s_type_map.erase(name);
}

DexFieldRef* RedexContext::make_field(const DexType* container,
                                      const DexString* name,
                                      const DexType* type) {
  always_assert(container != nullptr && name != nullptr && type != nullptr);
  DexFieldSpec r(const_cast<DexType*>(container), name,
                 const_cast<DexType*>(type));
  auto rv = s_field_map.get(r, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  std::unique_ptr<DexField> field(new DexField(
      const_cast<DexType*>(container), name, const_cast<DexType*>(type)));
  return try_insert<DexField, DexFieldRef>(r, std::move(field), &s_field_map);
}

DexFieldRef* RedexContext::get_field(const DexType* container,
                                     const DexString* name,
                                     const DexType* type) {
  if (container == nullptr || name == nullptr || type == nullptr) {
    return nullptr;
  }
  DexFieldSpec r(const_cast<DexType*>(container), name,
                 const_cast<DexType*>(type));
  return s_field_map.get(r, nullptr);
}

void RedexContext::alias_field_name(DexFieldRef* field,
                                    const DexString* new_name) {
  DexFieldSpec r(field->m_spec.cls, new_name, field->m_spec.type);
  always_assert_log(
      !s_field_map.count(r),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  s_field_map.emplace(r, field);
}

void RedexContext::erase_field(DexFieldRef* field) {
  s_field_map.erase(field->m_spec);
}

void RedexContext::erase_field(const DexType* container,
                               const DexString* name,
                               const DexType* type) {
  DexFieldSpec r(const_cast<DexType*>(container), name,
                 const_cast<DexType*>(type));
  s_field_map.erase(r);
}

void RedexContext::mutate_field(DexFieldRef* field,
                                const DexFieldSpec& ref,
                                bool rename_on_collision) {
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
}

DexTypeList* RedexContext::make_type_list(
    RedexContext::DexTypeListContainerType&& p) {
  auto rv = s_typelist_map.get(&p, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  std::unique_ptr<DexTypeList> typelist(new DexTypeList(std::move(p)));
  auto key = &typelist->m_list;
  return try_insert(key, std::move(typelist), &s_typelist_map);
}

DexTypeList* RedexContext::get_type_list(
    const RedexContext::DexTypeListContainerType& p) {
  return s_typelist_map.get(&p, nullptr);
}

size_t RedexContext::DexProtoKeyHash::operator()(DexProto* k) const {
  return (size_t)k->get_rtype() ^ (size_t)k->get_args();
}
bool RedexContext::DexProtoKeyEqual::operator()(DexProto* a,
                                                DexProto* b) const {
  return a->get_rtype() == b->get_rtype() && a->get_args() == b->get_args();
}

DexProto* RedexContext::make_proto(const DexType* rtype,
                                   const DexTypeList* args,
                                   const DexString* shorty) {
  always_assert(rtype != nullptr && args != nullptr && shorty != nullptr);
  DexProto key(const_cast<DexType*>(rtype), const_cast<DexTypeList*>(args),
               nullptr);
  auto rv_ptr = s_proto_set.get(&key);
  if (rv_ptr != nullptr) {
    return const_cast<DexProto*>(*rv_ptr);
  }
  std::unique_ptr<DexProto> proto(new DexProto(
      const_cast<DexType*>(rtype), const_cast<DexTypeList*>(args), shorty));
  return const_cast<DexProto*>(*try_insert(std::move(proto), &s_proto_set));
}

DexProto* RedexContext::get_proto(const DexType* rtype,
                                  const DexTypeList* args) {
  if (rtype == nullptr || args == nullptr) {
    return nullptr;
  }
  DexProto key(const_cast<DexType*>(rtype), const_cast<DexTypeList*>(args),
               nullptr);
  auto rv_ptr = s_proto_set.get(&key);
  return rv_ptr == nullptr ? nullptr : const_cast<DexProto*>(*rv_ptr);
}

DexMethodRef* RedexContext::make_method(const DexType* type_,
                                        const DexString* name_,
                                        const DexProto* proto_) {
  // Ideally, DexMethodSpec would store const types, then these casts wouldn't
  // be necessary, but that would involve cleaning up quite a bit of existing
  // code.
  auto type = const_cast<DexType*>(type_);
  auto name = name_;
  auto proto = const_cast<DexProto*>(proto_);
  always_assert(type != nullptr && name != nullptr && proto != nullptr);
  DexMethodSpec r(type, name, proto);
  auto rv = s_method_map.get(r, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  std::unique_ptr<DexMethod, DexMethod::Deleter> method(
      new DexMethod(type, name, proto));
  return try_insert<DexMethod, DexMethodRef>(r, std::move(method),
                                             &s_method_map);
}

DexMethodRef* RedexContext::get_method(const DexType* type,
                                       const DexString* name,
                                       const DexProto* proto) {
  if (type == nullptr || name == nullptr || proto == nullptr) {
    return nullptr;
  }
  DexMethodSpec r(const_cast<DexType*>(type), name,
                  const_cast<DexProto*>(proto));
  return s_method_map.get(r, nullptr);
}

void RedexContext::alias_method_name(DexMethodRef* method,
                                     const DexString* new_name) {
  DexMethodSpec r(method->m_spec.cls, new_name, method->m_spec.proto);
  always_assert_log(
      !s_method_map.count(r),
      "Bailing, attempting to alias a symbol that already exists! '%s'\n",
      new_name->c_str());
  s_method_map.emplace(r, method);
}

void RedexContext::erase_method(DexMethodRef* method) {
  s_method_map.erase(method->m_spec);
  // Also remove the alias from the map
  if (method->is_def()) {
    if (method->DexMethodRef::as_def()->get_deobfuscated_name_or_null() !=
        nullptr) {
      DexMethodSpec r(method->m_spec.cls,
                      &method->DexMethodRef::as_def()->get_deobfuscated_name(),
                      method->m_spec.proto);
      s_method_map.erase(r);
    }
  }
}

void RedexContext::erase_method(const DexType* type,
                                const DexString* name,
                                const DexProto* proto) {
  DexMethodSpec r(const_cast<DexType*>(type), name,
                  const_cast<DexProto*>(proto));
  s_method_map.erase(r);
}

// TODO: Need a better interface.
void RedexContext::mutate_method(DexMethodRef* method,
                                 const DexMethodSpec& new_spec,
                                 bool rename_on_collision) {
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
    if (new_spec.cls == nullptr || new_spec.cls == old_spec.cls) {
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
      // We are about to change its class. Use a better name to remember its
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
    }
  }

  // We might still miss name collision cases. As of now, let's just assert.
  if (s_method_map.count(r)) {
    always_assert_log(!s_method_map.count(r),
                      "Another method of the same signature already exists %s"
                      " %s %s",
                      SHOW(r.cls), SHOW(r.name), SHOW(r.proto));
  }
  s_method_map.emplace(r, method);
}

DexLocation* RedexContext::make_location(std::string_view store_name,
                                         std::string_view file_name) {
  auto key = std::make_pair(store_name, file_name);
  auto rv = s_location_map.get(key, nullptr);
  if (rv != nullptr) {
    return rv;
  }
  std::unique_ptr<DexLocation> value(
      new DexLocation(std::string(store_name), std::string(file_name)));
  key = std::make_pair(value->get_store_name(), value->get_file_name());
  return try_insert(key, std::move(value), &s_location_map);
}

DexLocation* RedexContext::get_location(std::string_view store_name,
                                        std::string_view file_name) {
  auto key = std::make_pair(store_name, file_name);
  return s_location_map.get(key, nullptr);
}

PositionPatternSwitchManager*
RedexContext::get_position_pattern_switch_manager() {
  if (!m_position_pattern_switch_manager) {
    m_position_pattern_switch_manager = new PositionPatternSwitchManager();
  }
  return m_position_pattern_switch_manager;
}

// Return false on unique classes
// Return true on benign duplicate classes
// Throw RedexException on problematic duplicate classes
bool RedexContext::class_already_loaded(DexClass* cls) {
  std::lock_guard<std::mutex> l(m_type_system_mutex);
  const DexType* type = cls->get_type();
  const auto& it = m_type_to_class.find(type);
  if (it == m_type_to_class.end()) {
    return false;
  } else {
    const auto& prev_loc = it->second->get_location()->get_file_name();
    const auto& cur_loc = cls->get_location()->get_file_name();
    if (prev_loc == cur_loc || dup_classes::is_known_dup(cls)) {
      // benign duplicates
      TRACE(MAIN, 1, "Warning: found a duplicate class: %s", SHOW(cls));
    } else {
      const std::string& class_name = show(cls);
      TRACE(MAIN,
            1,
            "Found a duplicate class: %s in two dexes:\ndex 1: %s\ndex "
            "2: %s\n",
            class_name.c_str(),
            prev_loc.c_str(),
            cur_loc.c_str());

      if (!m_allow_class_duplicates) {
        throw RedexException(
            RedexError::DUPLICATE_CLASSES,
            "Found duplicate class in two different files.",
            {{"class", class_name}, {"dex1", prev_loc}, {"dex2", cur_loc}});
      }
    }
    return true;
  }
}

void RedexContext::publish_class(DexClass* cls) {
  std::lock_guard<std::mutex> l(m_type_system_mutex);
  const DexType* type = cls->get_type();
  const auto& pair = m_type_to_class.emplace(type, cls);
  bool insertion_took_place = pair.second;
  always_assert_log(insertion_took_place,
                    "No insertion for class: %s with deobfuscated name: %s",
                    cls->get_name()->c_str(),
                    cls->get_deobfuscated_name().c_str());
  if (cls->is_external()) {
    m_external_classes.emplace_back(cls);
  }
}

DexClass* RedexContext::type_class(const DexType* t) {
  auto it = m_type_to_class.find(t);
  return it != m_type_to_class.end() ? it->second : nullptr;
}

void RedexContext::set_field_value(DexField* field,
                                   keep_rules::AssumeReturnValue& val) {
  field_values.emplace(field,
                       std::make_unique<keep_rules::AssumeReturnValue>(val));
}

keep_rules::AssumeReturnValue* RedexContext::get_field_value(DexField* field) {
  auto it = field_values.find(field);
  if (it != field_values.end()) {
    return it->second.get();
  }
  return nullptr;
}

void RedexContext::unset_field_value(DexField* field) {
  field_values.erase(field);
}

void RedexContext::set_return_value(DexMethod* method,
                                    keep_rules::AssumeReturnValue& val) {
  method_return_values.emplace(
      method, std::make_unique<keep_rules::AssumeReturnValue>(val));
}

keep_rules::AssumeReturnValue* RedexContext::get_return_value(
    DexMethod* method) {
  auto it = method_return_values.find(method);
  if (it != method_return_values.end()) {
    return it->second.get();
  }
  return nullptr;
}

void RedexContext::unset_return_value(DexMethod* method) {
  method_return_values.erase(method);
}

void RedexContext::add_destruction_task(const Task& t) {
  std::unique_lock<std::mutex> lock{m_destruction_tasks_lock};
  m_destruction_tasks.push_back(t);
}

void RedexContext::set_sb_interaction_index(
    const std::unordered_map<std::string, size_t>& input) {
  m_sb_interaction_indices = input;
}
