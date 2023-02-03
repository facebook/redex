/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Debug.h"
#include "Timer.h"

// Forward declaration.
namespace cc_impl {

template <typename Container, size_t n_slots>
class ConcurrentContainerIterator;

inline AccumulatingTimer s_destructor{};

inline double get_destructor_seconds() { return s_destructor.get_seconds(); }

void workqueue_run_for(size_t start,
                       size_t end,
                       const std::function<void(size_t)>& fn);

} // namespace cc_impl

/*
 * This class implements the common functionalities of concurrent sets and maps.
 * A concurrent container is just a collection of STL hash maps/sets
 * (unordered_map/unordered_set) arranged in slots. Whenever a thread performs a
 * concurrent operation on an element, the slot uniquely determined by the hash
 * code of the element is locked and the corresponding operation is performed on
 * the underlying STL container. This is a very simple design, which offers
 * reasonable performance in practice. A high number of slots may help reduce
 * thread contention at the expense of a larger memory footprint. It is advised
 * to use a prime number for `n_slots`, so as to ensure a more even spread of
 * elements across slots.
 *
 * There are two major modes in which a concurrent container is thread-safe:
 *  - Read only: multiple threads access the contents of the container but do
 *    not attempt to modify any element.
 *  - Write only: multiple threads update the contents of the container but do
 *    not otherwise attempt to access any element.
 * The few operations that are thread-safe regardless of the access mode are
 * documented as such.
 */
template <typename Container, typename Key, typename Hash, size_t n_slots>
class ConcurrentContainer {
 public:
  static_assert(n_slots > 0, "The concurrent container has no slots");

  using iterator = cc_impl::ConcurrentContainerIterator<Container, n_slots>;

  using const_iterator =
      cc_impl::ConcurrentContainerIterator<const Container, n_slots>;

  virtual ~ConcurrentContainer() {
    auto timer_scope = cc_impl::s_destructor.scope();
    if (size() < 4096) {
      for (size_t slot = 0; slot < n_slots; ++slot) {
        m_slots[slot] = Container();
      }
      return;
    }
    cc_impl::workqueue_run_for(
        0, n_slots, [this](size_t slot) { m_slots[slot] = Container(); });
  }

  /*
   * Using iterators or accessor functions while the container is concurrently
   * modified will result in undefined behavior.
   */

  iterator begin() { return iterator(&m_slots[0], 0, m_slots[0].begin()); }

  iterator end() { return iterator(&m_slots[0]); }

  const_iterator begin() const {
    return const_iterator(&m_slots[0], 0, m_slots[0].begin());
  }

  const_iterator end() const { return const_iterator(&m_slots[0]); }

  const_iterator cbegin() const { return begin(); }

  const_iterator cend() const { return end(); }

  iterator find(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    const auto& it = m_slots[slot].find(key);
    if (it == m_slots[slot].end()) {
      return end();
    }
    return iterator(&m_slots[0], slot, it);
  }

  const_iterator find(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& it = m_slots[slot].find(key);
    if (it == m_slots[slot].end()) {
      return end();
    }
    return const_iterator(&m_slots[0], slot, it);
  }

  size_t size() const {
    size_t s = 0;
    for (size_t slot = 0; slot < n_slots; ++slot) {
      s += m_slots[slot].size();
    }
    return s;
  }

  bool empty() const {
    for (size_t slot = 0; slot < n_slots; ++slot) {
      if (!m_slots[slot].empty()) {
        return false;
      }
    }
    return true;
  }

  void reserve(size_t capacity) {
    size_t slot_capacity = capacity / n_slots;
    if (slot_capacity > 0) {
      for (size_t i = 0; i < n_slots; ++i) {
        m_slots[i].reserve(slot_capacity);
      }
    }
  }

  void clear() {
    for (size_t slot = 0; slot < n_slots; ++slot) {
      m_slots[slot].clear();
    }
  }

  /*
   * This operation is always thread-safe.
   */
  size_t count(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock{m_locks[slot]};
    return m_slots[slot].count(key);
  }

  size_t count_unsafe(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    return m_slots[slot].count(key);
  }

  /*
   * This operation is always thread-safe.
   */
  size_t erase(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock{m_locks[slot]};
    return m_slots[slot].erase(key);
  }

  /*
   * This operation is not thread-safe.
   */
  size_t bucket_size(size_t i) const {
    always_assert(i < n_slots);
    return m_slots[i].size();
  }

  /*
   * WARNING: Only use with unsafe functions, or risk deadlock or undefined
   * behavior!
   */
  std::mutex& get_lock(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    return get_lock(slot);
  }

  /*
   * This operation is not thread-safe.
   */
  Container move_to_container() {
    Container res;
    res.reserve(size());
    for (size_t slot = 0; slot < n_slots; ++slot) {
      auto& c = m_slots[slot];
      res.insert(std::make_move_iterator(c.begin()),
                 std::make_move_iterator(c.end()));
      c.clear();
    }
    return res;
  }

 protected:
  // Only derived classes may be instantiated or copied.
  ConcurrentContainer() = default;

  ConcurrentContainer(const ConcurrentContainer& container) {
    for (size_t i = 0; i < n_slots; ++i) {
      m_slots[i] = container.m_slots[i];
    }
  }

  ConcurrentContainer(ConcurrentContainer&& container) noexcept {
    for (size_t i = 0; i < n_slots; ++i) {
      m_slots[i] = std::move(container.m_slots[i]);
    }
  }

  Container& get_container(size_t slot) { return m_slots[slot]; }

  const Container& get_container(size_t slot) const { return m_slots[slot]; }

  std::mutex& get_lock(size_t slot) const { return m_locks[slot]; }

 protected:
  mutable std::mutex m_locks[n_slots];
  Container m_slots[n_slots];
};

struct Identity {
  template <typename T>
  T&& operator()(T&& t) const {
    return std::forward<T>(t);
  }
};

// A concurrent container with map semantics.
//
// Note: `KeyProjection` allows to use a different key for the
//       `ConcurrentMapContainer` vs the internal sharded storage.
//
//       This is in general possible without, but may have storage overhead.
//       An example is a `pair` key, where the first component is
//       used for sharding, and the second component for the internal
//       map. This could be simulated by a `pair` key in the internal map
//       and corresponding hash/compare/equals functions,

template <typename MapContainer,
          typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyProjection = Identity,
          size_t n_slots = 31>
class ConcurrentMapContainer
    : public ConcurrentContainer<MapContainer, Key, Hash, n_slots> {
 public:
  using typename ConcurrentContainer<MapContainer, Key, Hash, n_slots>::
      const_iterator;
  using
      typename ConcurrentContainer<MapContainer, Key, Hash, n_slots>::iterator;

  using ConcurrentContainer<MapContainer, Key, Hash, n_slots>::m_slots;
  using ConcurrentContainer<MapContainer, Key, Hash, n_slots>::end;

  ConcurrentMapContainer() = default;

  ConcurrentMapContainer(const ConcurrentMapContainer& container)
      : ConcurrentContainer<MapContainer, Key, Hash, n_slots>(container) {}

  ConcurrentMapContainer(ConcurrentMapContainer&& container) noexcept
      : ConcurrentContainer<MapContainer, Key, Hash, n_slots>(
            std::move(container)) {}

  ConcurrentMapContainer& operator=(ConcurrentMapContainer&&) noexcept =
      default;

  ConcurrentMapContainer& operator=(const ConcurrentMapContainer&) noexcept =
      default;

  template <typename InputIt>
  ConcurrentMapContainer(InputIt first, InputIt last) {
    insert(first, last);
  }

  /*
   * This operation is always thread-safe. Note that it returns a copy of Value
   * rather than a reference since insertions from other threads may cause the
   * hashtables to be resized. If you are reading from a ConcurrentMap that is
   * not being concurrently modified, it will probably be faster to use
   * `find()` or `at_unsafe()` to avoid the copy.
   */
  Value at(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    return this->get_container(slot).at(KeyProjection()(key));
  }

  const Value& at_unsafe(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    return this->get_container(slot).at(KeyProjection()(key));
  }

  Value& at_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    return this->get_container(slot).at(KeyProjection()(key));
  }

  iterator find(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    const auto& it = m_slots[slot].find(KeyProjection()(key));
    if (it == m_slots[slot].end()) {
      return end();
    }
    return iterator(&m_slots[0], slot, it);
  }

  const_iterator find(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& it = m_slots[slot].find(KeyProjection()(key));
    if (it == m_slots[slot].end()) {
      return end();
    }
    return const_iterator(&m_slots[0], slot, it);
  }

  /*
   * This operation is always thread-safe.
   */
  Value get(const Key& key, Value default_value) const {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    const auto& map = this->get_container(slot);
    const auto& it = map.find(KeyProjection()(key));
    if (it == map.end()) {
      return default_value;
    }
    return it->second;
  }

  /*
   * The Boolean return value denotes whether the insertion took place.
   * This operation is always thread-safe.
   *
   * Note that while the STL containers' insert() methods return both an
   * iterator and a boolean success value, we only return the boolean value
   * here as any operations on a returned iterator are not guaranteed to be
   * thread-safe.
   */
  bool insert(const std::pair<Key, Value>& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    return map
        .insert(std::make_pair(KeyProjection()(entry.first), entry.second))
        .second;
  }

  /*
   * This operation is always thread-safe.
   */
  void insert(std::initializer_list<std::pair<Key, Value>> l) {
    for (const auto& entry : l) {
      insert(entry);
    }
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename InputIt>
  void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) {
      insert(*first);
    }
  }

  /*
   * This operation is always thread-safe.
   */
  void insert_or_assign(const std::pair<Key, Value>& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    map[KeyProjection()(entry.first)] = entry.second;
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  bool emplace(Args&&... args) {
    std::pair<Key, Value> entry(std::forward<Args>(args)...);
    size_t slot = Hash()(entry.first) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    return map
        .emplace(KeyProjection()(std::move(entry.first)),
                 std::move(entry.second))
        .second;
  }

  /*
   * This operation atomically modifies an entry in the map. If the entry
   * doesn't exist, it is created. The third argument of the updater function is
   * a Boolean flag denoting whether the entry exists or not.
   */
  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update(const Key& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    auto it = map.find(KeyProjection()(key));
    if (it == map.end()) {
      updater(KeyProjection()(key), map[KeyProjection()(key)], false);
    } else {
      updater(it->first, it->second, true);
    }
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update_unsafe(const Key& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto it = map.find(KeyProjection()(key));
    if (it == map.end()) {
      updater(KeyProjection()(key), map[KeyProjection()(key)], false);
    } else {
      updater(it->first, it->second, true);
    }
  }
};

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>,
          size_t n_slots = 31>
using ConcurrentMap =
    ConcurrentMapContainer<std::unordered_map<Key, Value, Hash, Equal>,
                           Key,
                           Value,
                           Hash,
                           Identity,
                           n_slots>;

template <typename Key,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>,
          size_t n_slots = 31>
class ConcurrentSet final
    : public ConcurrentContainer<std::unordered_set<Key, Hash, Equal>,
                                 Key,
                                 Hash,
                                 n_slots> {
 public:
  ConcurrentSet() = default;

  ConcurrentSet(const ConcurrentSet& set)
      : ConcurrentContainer<std::unordered_set<Key, Hash, Equal>,
                            Key,
                            Hash,
                            n_slots>(set) {}

  ConcurrentSet(ConcurrentSet&& set) noexcept
      : ConcurrentContainer<std::unordered_set<Key, Hash, Equal>,
                            Key,
                            Hash,
                            n_slots>(std::move(set)) {}

  ConcurrentSet& operator=(ConcurrentSet&&) noexcept = default;

  ConcurrentSet& operator=(const ConcurrentSet&) noexcept = default;

  /*
   * The Boolean return value denotes whether the insertion took place.
   * This operation is always thread-safe.
   */
  bool insert(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& set = this->get_container(slot);
    return set.insert(key).second;
  }

  /*
   * This operation is always thread-safe.
   */
  void insert(std::initializer_list<Key> l) {
    for (const auto& x : l) {
      insert(x);
    }
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename InputIt>
  void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) {
      insert(*first);
    }
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  bool emplace(Args&&... args) {
    Key key(std::forward<Args>(args)...);
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& set = this->get_container(slot);
    return set.emplace(std::move(key)).second;
  }
};

/**
// A concurrent container with set semantics that only accepts insertions.
 *
 * This allows accessing constant references on elements safely.
 */
template <typename SetContainer,
          typename Key,
          typename Hash = std::hash<Key>,
          size_t n_slots = 31>
class InsertOnlyConcurrentSetContainer final
    : public ConcurrentContainer<SetContainer, Key, Hash, n_slots> {
 public:
  InsertOnlyConcurrentSetContainer() = default;

  InsertOnlyConcurrentSetContainer(const InsertOnlyConcurrentSetContainer& set)
      : ConcurrentContainer<SetContainer, Key, Hash, n_slots>(set) {}

  InsertOnlyConcurrentSetContainer(
      InsertOnlyConcurrentSetContainer&& set) noexcept
      : ConcurrentContainer<SetContainer, Key, Hash, n_slots>(std::move(set)) {}

  InsertOnlyConcurrentSetContainer& operator=(
      InsertOnlyConcurrentSetContainer&&) noexcept = default;

  InsertOnlyConcurrentSetContainer& operator=(
      const InsertOnlyConcurrentSetContainer&) noexcept = default;

  /*
   * Returns a pair consisting of a pointer on the inserted element (or the
   * element that prevented the insertion) and a boolean denoting whether the
   * insertion took place. This operation is always thread-safe.
   */
  std::pair<const Key*, bool> insert(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    auto& set = this->get_container(slot);
    // `std::unordered_set::insert` does not invalidate references,
    // thus it is safe to return a reference on the object.
    auto result = set.insert(key);
    return {&*result.first, result.second};
  }

  /*
   * Return a pointer on the element, or `nullptr` if the element is not in the
   * set. This operation is always thread-safe.
   */
  const Key* get(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    std::unique_lock<std::mutex> lock(this->get_lock(slot));
    const auto& set = this->get_container(slot);
    auto result = set.find(key);
    if (result == set.end()) {
      return nullptr;
    } else {
      return &*result;
    }
  }

  size_t erase(const Key& key) = delete;
};

/**
 * A concurrent set that only accepts insertions.
 *
 * This allows accessing constant references on elements safely.
 */
template <typename Key,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>,
          size_t n_slots = 31>
using InsertOnlyConcurrentSet =
    InsertOnlyConcurrentSetContainer<std::unordered_set<Key, Hash, Equal>,
                                     Key,
                                     Hash,
                                     n_slots>;

namespace cc_impl {

template <typename Container, size_t n_slots>
class ConcurrentContainerIterator final {
 public:
  using base_iterator = std::conditional_t<std::is_const<Container>::value,
                                           typename Container::const_iterator,
                                           typename Container::iterator>;
  using const_base_iterator = typename Container::const_iterator;
  using difference_type = std::ptrdiff_t;
  using value_type = typename base_iterator::value_type;
  using pointer = typename base_iterator::pointer;
  using const_pointer = typename Container::const_iterator::pointer;
  using reference = typename base_iterator::reference;
  using const_reference = typename Container::const_iterator::reference;
  using iterator_category = std::forward_iterator_tag;

  explicit ConcurrentContainerIterator(Container* slots)
      : m_slots(slots),
        m_slot(n_slots - 1),
        m_position(m_slots[n_slots - 1].end()) {
    skip_empty_slots();
  }

  ConcurrentContainerIterator(Container* slots,
                              size_t slot,
                              const base_iterator& position)
      : m_slots(slots), m_slot(slot), m_position(position) {
    skip_empty_slots();
  }

  ConcurrentContainerIterator& operator++() {
    always_assert(m_position != m_slots[n_slots - 1].end());
    ++m_position;
    skip_empty_slots();
    return *this;
  }

  ConcurrentContainerIterator operator++(int) {
    ConcurrentContainerIterator retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(const ConcurrentContainerIterator& other) const {
    return m_slots == other.m_slots && m_slot == other.m_slot &&
           m_position == other.m_position;
  }

  bool operator!=(const ConcurrentContainerIterator& other) const {
    return !(*this == other);
  }

  reference operator*() {
    always_assert(m_position != m_slots[n_slots - 1].end());
    return *m_position;
  }

  pointer operator->() {
    always_assert(m_position != m_slots[n_slots - 1].end());
    return m_position.operator->();
  }

  const_reference operator*() const {
    always_assert(m_position != m_slots[n_slots - 1].end());
    return *m_position;
  }

  const_pointer operator->() const {
    always_assert(m_position != m_slots[n_slots - 1].end());
    return m_position.operator->();
  }

 private:
  void skip_empty_slots() {
    while (m_position == m_slots[m_slot].end() && m_slot < n_slots - 1) {
      m_position = m_slots[++m_slot].begin();
    }
  }

  Container* m_slots;
  size_t m_slot;
  base_iterator m_position;
};

} // namespace cc_impl
