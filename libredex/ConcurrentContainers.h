/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <boost/thread.hpp>

#include "Debug.h"

// Forward declaration.
namespace cc_impl {

template <typename Container, typename Iterator, size_t n_slots>
class ConcurrentContainerIterator;

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

  using iterator =
      cc_impl::ConcurrentContainerIterator<Container,
                                           typename Container::iterator,
                                           n_slots>;

  using const_iterator =
      cc_impl::ConcurrentContainerIterator<Container,
                                           typename Container::const_iterator,
                                           n_slots>;

  virtual ~ConcurrentContainer() {}

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

  size_t size() const {
    size_t s = 0;
    for (size_t slot = 0; slot < n_slots; ++slot) {
      s += m_slots[slot].size();
    }
    return s;
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
  size_t count(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    boost::lock_guard<boost::mutex> lock(m_locks[slot]);
    return m_slots[slot].count(key);
  }

  /*
   * This operation is always thread-safe.
   */
  size_t erase(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    boost::lock_guard<boost::mutex> lock(m_locks[slot]);
    return m_slots[slot].erase(key);
  }

 protected:
  // Only derived classes may be instantiated.
  ConcurrentContainer() = default;

  Container& get_container(size_t slot) { return m_slots[slot]; }

  boost::mutex& get_lock(size_t slot) { return m_locks[slot]; }

 private:
  boost::mutex m_locks[n_slots];
  Container m_slots[n_slots];
};

template <typename Key,
          typename Value,
          size_t n_slots = 31,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class ConcurrentMap final
    : public ConcurrentContainer<std::unordered_map<Key, Value, Hash, Equal>,
                                 Key,
                                 Hash,
                                 n_slots> {
 public:
  /*
   * The Boolean return value denotes whether the insertion took place.
   * This operation is always thread-safe.
   */
  bool insert(const std::pair<Key, Value>& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    boost::lock_guard<boost::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    auto status = map.insert(entry);
    return status.second;
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
   * This operation atomically modifies an entry in the map. If the entry
   * doesn't exist, it is created. The third argument of the updater function is
   * a Boolean flag denoting whether the entry exists or not.
   */
  void update(const Key& key,
              const std::function<void(const Key&, Value&, bool)>& updater) {
    size_t slot = Hash()(key) % n_slots;
    boost::lock_guard<boost::mutex> lock(this->get_lock(slot));
    auto& map = this->get_container(slot);
    auto it = map.find(key);
    if (it == map.end()) {
      updater(key, map[key], false);
    } else {
      updater(it->first, it->second, true);
    }
  }
};

template <typename Key,
          size_t n_slots = 31,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class ConcurrentSet final
    : public ConcurrentContainer<std::unordered_set<Key, Hash, Equal>,
                                 Key,
                                 Hash,
                                 n_slots> {
 public:
  /*
   * The Boolean return value denotes whether the insertion took place.
   * This operation is always thread-safe.
   */
  bool insert(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    boost::lock_guard<boost::mutex> lock(this->get_lock(slot));
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
};

namespace cc_impl {

template <typename Container, typename Iterator, size_t n_slots>
class ConcurrentContainerIterator final
    : public std::iterator<std::forward_iterator_tag,
                           typename Iterator::value_type> {
 public:
  explicit ConcurrentContainerIterator(Container* slots)
      : m_slots(slots),
        m_slot(n_slots - 1),
        m_position(m_slots[n_slots - 1].end()) {
    skip_empty_slots();
  }

  ConcurrentContainerIterator(Container* slots,
                              size_t slot,
                              const Iterator& position)
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

  typename Iterator::reference operator*() {
    always_assert(m_position != m_slots[n_slots - 1].end());
    return *m_position;
  }

  typename Iterator::pointer operator->() {
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
  Iterator m_position;
};

} // namespace cc_impl
