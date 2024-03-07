/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/intrusive/pointer_plus_bits.hpp>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Debug.h"
#include "Timer.h"

namespace cc_impl {

constexpr size_t kDefaultSlots = 83;

template <typename Container, size_t n_slots>
class ConcurrentContainerIterator;

inline AccumulatingTimer s_destructor("cc_impl::destructor_seconds");
inline AccumulatingTimer s_reserving("cc_impl::reserving_seconds");

inline size_t s_concurrent_destruction_threshold{
    std::numeric_limits<size_t>::max()};

bool is_thread_pool_active();

void workqueue_run_for(size_t start,
                       size_t end,
                       const std::function<void(size_t)>& fn);

template <typename ConcurrentHashtable>
class ConcurrentHashtableIterator;

template <typename ConcurrentHashtable>
class ConcurrentHashtableInsertionResult;

size_t get_prime_number_greater_or_equal_to(size_t);

/*
 * This ConcurrentHashtable supports inserting (and "emplacing"), getting (the
 * address of inserted key-value pairs), and erasing key-value pairs. There is
 * no built-in support for mutation of previously inserted elements; however,
 * once inserted, a key-value is assigned a fixed storage location that will
 * remain valid until the concurrent hashtable is destroyed, or a destructive
 * NOT thread-safe function such as `compact` is called.
 *
 * Some guiding principles for concurrency are:
 * - All insertions/erasures performed on the current thread are reflected when
 *   calling get on the current thread.
 * - Insertions/erasures performed by other threads will become visible
 *   eventually, but with no ordering guarantees.
 *
 * The concurrent hashtable has the following performance characteristics:
 * - getting, inserting and erasing is O(1) on average, lock-free (*), and not
 *   blocked by resizing
 * - resizing is O(n) on the current thread, and acquires a table-wide mutex
 *
 * (*) While implemented without locks, there is effectively some spinning on
 * individual buckets when competing operations are in progress on that bucket.
 *
 * Resizing is automatically triggered when an insertion causes the table to
 * exceed the (hard-coded) load factor, and then this insertion blocks the
 * current thread while it is busy resizing. However, concurrent gets,
 * insertions and erasures can proceed; new insertions will go into the enlarged
 * table version, possibly (temporarily) exceeding the load factor.
 *
 * All key-value pairs are stored in a fixed memory location, and are not moved
 * during resizing, similar to how std::unordered_set/map manages memory.
 * Erasing a key does not immediately destroy the key-value pair, but keeps (a
 * reference to) it until the concurrent hashtable is destroyed, copied, moved,
 * or `compact` is called. This ensures that get always returns a valid
 * reference, even in the face of concurrent erasing.
 *
 * TODO: Right now, we use the (default) std::memory_order_seq_cst everywhere.
 * Acquire/release semantics should be sufficient.
 */
template <typename Key, typename Value, typename Hash, typename KeyEqual>
class ConcurrentHashtable final {
 public:
  using key_type = Key;
  using value_type = Value;
  using pointer = Value*;
  using const_pointer = const Value*;
  using reference = Value&;
  using const_reference = const Value&;
  using iterator = ConcurrentHashtableIterator<
      ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;
  using const_iterator = ConcurrentHashtableIterator<
      const ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using insertion_result = ConcurrentHashtableInsertionResult<
      ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;

  struct const_key_projection {
    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  std::is_same_v<key_type2, value_type>>>
    const key_type2& operator()(const key_type2& key) {
      return key;
    }

    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  !std::is_same_v<key_type2, value_type>>>
    const key_type2& operator()(const value_type& key) {
      return key.first;
    }
  };

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  iterator begin() {
    auto* storage = m_storage.load();
    auto* ptr = storage->ptrs[0].load();
    return iterator(storage, 0, get_node(ptr));
  }

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  iterator end() {
    auto* storage = m_storage.load();
    return iterator(storage, storage->size, nullptr);
  }

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  const_iterator begin() const {
    auto* storage = m_storage.load();
    auto* ptr = storage->ptrs[0].load();
    return const_iterator(storage, 0, get_node(ptr));
  }

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  const_iterator end() const {
    auto* storage = m_storage.load();
    return const_iterator(storage, storage->size, nullptr);
  }

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  iterator find(const key_type& key) {
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    auto* ptrs = storage->ptrs;
    size_t i = hash % storage->size;
    auto* root_loc = &ptrs[i];
    auto* root = root_loc->load();
    for (auto* ptr = root; ptr;) {
      auto* node = get_node(ptr);
      if (key_equal()(const_key_projection()(node->value), key)) {
        return iterator(storage, i, node);
      }
      ptr = node->prev.load();
    }
    return end();
  }

  /*
   * This operation by itself is always thread-safe. However, any mutating
   * operations (concurrent or synchronous) invalidate all iterators.
   */
  const_iterator find(const key_type& key) const {
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    auto* ptrs = storage->ptrs;
    size_t i = hash % storage->size;
    auto* root_loc = &ptrs[i];
    auto* root = root_loc->load();
    for (auto* ptr = root; ptr;) {
      auto* node = get_node(ptr);
      if (key_equal()(const_key_projection()(node->value), key)) {
        return const_iterator(storage, i, node);
      }
      ptr = node->prev.load();
    }
    return end();
  }

  /*
   * This operation is always thread-safe.
   */
  size_t size() const { return m_count.load(); }

  /*
   * This operation is always thread-safe.
   */
  bool empty() const { return size() == 0; }

  /*
   * This operation is NOT thread-safe.
   */
  void clear(size_t size = INITIAL_SIZE) {
    if (m_count.load() > 0) {
      Storage::destroy(m_storage.exchange(Storage::create(size, nullptr)));
      m_count.store(0);
    }
    compact();
  }

  ConcurrentHashtable() noexcept
      : m_storage(Storage::create()), m_count(0), m_erased(nullptr) {}

  /*
   * This operation is NOT thread-safe.
   */
  ConcurrentHashtable(const ConcurrentHashtable& container) noexcept
      : m_storage(Storage::create(container.size() / LOAD_FACTOR + 1, nullptr)),
        m_count(0),
        m_erased(nullptr) {
    for (const auto& p : container) {
      try_insert(p);
    }
  }

  /*
   * This operation is NOT thread-safe.
   */
  ConcurrentHashtable(ConcurrentHashtable&& container) noexcept
      : m_storage(container.m_storage.exchange(Storage::create())),
        m_count(container.m_count.exchange(0)),
        m_erased(container.m_erased.exchange(nullptr)) {
    compact();
  }

  /*
   * This operation is NOT thread-safe.
   */
  ConcurrentHashtable& operator=(ConcurrentHashtable&& container) noexcept {
    clear();
    container.compact();
    m_storage.store(container.m_storage.exchange(m_storage.load()));
    m_count.store(container.m_count.exchange(0));
    return *this;
  }

  /*
   * This operation is NOT thread-safe.
   */
  ConcurrentHashtable& operator=(
      const ConcurrentHashtable& container) noexcept {
    if (this != &container) {
      clear(container.size() / LOAD_FACTOR + 1);
      for (const auto& p : container) {
        try_insert(p);
      }
    }
    return *this;
  }

  /*
   * This operation releases all memory and leaves behind the object in an
   * uninitialized state.
   */
  void destroy() {
    Storage::destroy(m_storage.exchange(nullptr));
    m_count.store(0);
    process_erased();
  }

  ~ConcurrentHashtable() { destroy(); }

  /*
   * This operation is always thread-safe.
   */
  value_type* get(const key_type& key) {
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    do {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value), key)) {
          return &node->value;
        }
      }
      storage = storage->next.load();
    } while (storage);
    return nullptr;
  }

  /*
   * This operation is always thread-safe.
   */
  const value_type* get(const key_type& key) const {
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    do {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value), key)) {
          return &node->value;
        }
      }
      storage = storage->next.load();
    } while (storage);
    return nullptr;
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  insertion_result try_emplace(const key_type& key, Args&&... args) {
    Node* new_node = nullptr;
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    while (true) {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value), key)) {
          return insertion_result(&node->value, new_node);
        }
      }
      if (is_moved_or_locked(root)) {
        if (auto* next_storage = storage->next.load()) {
          storage = next_storage;
          continue;
        }
        // We are racing with an erasure; assume it's not affecting us.
        root = get_node(root);
      }
      if (load_factor_exceeded(storage) && reserve(storage->size * 2)) {
        storage = m_storage.load();
        continue;
      }
      if (!new_node) {
        new_node =
            new Node(ConstRefKeyArgsTag(), key, std::forward<Args>(args)...);
      }
      new_node->prev = root;
      if (root_loc->compare_exchange_strong(root, new_node)) {
        m_count.fetch_add(1);
        return insertion_result(&new_node->value);
      }
      // We lost a race with another insertion
    }
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  insertion_result try_emplace(key_type&& key, Args&&... args) {
    Node* new_node = nullptr;
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    const key_type* key_ptr = &key;
    while (true) {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value), *key_ptr)) {
          return insertion_result(&node->value, new_node);
        }
      }
      if (is_moved_or_locked(root)) {
        if (auto* next_storage = storage->next.load()) {
          storage = next_storage;
          continue;
        }
        // We are racing with an erasure; assume it's not affecting us.
        root = get_node(root);
      }
      if (load_factor_exceeded(storage) && reserve(storage->size * 2)) {
        storage = m_storage.load();
        continue;
      }
      if (!new_node) {
        new_node = new Node(RvalueRefKeyArgsTag(), std::forward<key_type>(key),
                            std::forward<Args>(args)...);
        key_ptr = &const_key_projection()(new_node->value);
      }
      new_node->prev = root;
      if (root_loc->compare_exchange_strong(root, new_node)) {
        m_count.fetch_add(1);
        return insertion_result(&new_node->value);
      }
      // We lost a race with another insertion
    }
  }

  /*
   * This operation is always thread-safe.
   */
  insertion_result try_insert(const value_type& value) {
    Node* new_node = nullptr;
    auto hash = hasher()(const_key_projection()(value));
    auto* storage = m_storage.load();
    while (true) {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value),
                        const_key_projection()(value))) {
          return insertion_result(&node->value, new_node);
        }
      }
      if (is_moved_or_locked(root)) {
        if (auto* next_storage = storage->next.load()) {
          storage = next_storage;
          continue;
        }
        // We are racing with an erasure; assume it's not affecting us.
        root = get_node(root);
      }
      if (load_factor_exceeded(storage) && reserve(storage->size * 2)) {
        storage = m_storage.load();
        continue;
      }
      if (!new_node) {
        new_node = new Node(ConstRefValueTag(), value);
      }
      new_node->prev = root;
      if (root_loc->compare_exchange_strong(root, new_node)) {
        m_count.fetch_add(1);
        return insertion_result(&new_node->value);
      }
      // We lost a race with another insertion
    }
  }

  /*
   * This operation is always thread-safe.
   */
  insertion_result try_insert(value_type&& value) {
    Node* new_node = nullptr;
    auto hash = hasher()(const_key_projection()(value));
    auto* storage = m_storage.load();
    auto* value_ptr = &value;
    while (true) {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      for (auto* node = get_node(root); node;
           node = get_node(node->prev.load())) {
        if (key_equal()(const_key_projection()(node->value),
                        const_key_projection()(*value_ptr))) {
          // We lost a race with an equivalent insertion
          return insertion_result(&node->value, new_node);
        }
      }
      if (is_moved_or_locked(root)) {
        if (auto* next_storage = storage->next.load()) {
          storage = next_storage;
          continue;
        }
        // We are racing with an erasure; assume it's not affecting us.
        root = get_node(root);
      }
      if (load_factor_exceeded(storage) && reserve(storage->size * 2)) {
        storage = m_storage.load();
        continue;
      }
      if (!new_node) {
        new_node =
            new Node(RvalueRefValueTag(), std::forward<value_type>(value));
        value_ptr = &new_node->value;
      }
      new_node->prev = root;
      if (root_loc->compare_exchange_strong(root, new_node)) {
        m_count.fetch_add(1);
        return insertion_result(&new_node->value);
      }
      // We lost a race with another insertion
    }
  }

  /*
   * This operation is always thread-safe.
   */
  bool reserve(size_t capacity) {
    bool resizing = false;
    if (!m_resizing.compare_exchange_strong(resizing, true)) {
      return false;
    }
    auto* storage = m_storage.load();
    if (storage->size >= capacity) {
      m_resizing.store(false);
      return true;
    }
    auto timer_scope = s_reserving.scope();
    auto new_capacity = get_prime_number_greater_or_equal_to(capacity);
    auto* ptrs = storage->ptrs;
    auto* new_storage = Storage::create(new_capacity, storage);
    storage->next.store(new_storage);
    std::stack<std::atomic<Ptr>*> locs;
    for (size_t i = 0; i < storage->size; ++i) {
      std::atomic<Ptr>* loc = &ptrs[i];
      // Lock the bucket (or mark the bucket as moved if its empty). This might
      // fail due to a race with an insertion or erasure
      Ptr ptr = nullptr;
      Node* node = nullptr;
      while (!loc->compare_exchange_strong(ptr, moved_or_lock(node))) {
        node = get_node(ptr);
        ptr = node;
      }
      if (node == nullptr) {
        continue;
      }
      // Lets rewire the nodes from the back to the new storage version.
      locs.push(loc);
      auto* prev_loc = &node->prev;
      auto* prev_ptr = prev_loc->load();
      while (prev_ptr) {
        loc = prev_loc;
        locs.push(loc);
        ptr = prev_ptr;
        node = get_node(ptr);
        prev_loc = &node->prev;
        prev_ptr = prev_loc->load();
      }
      while (!locs.empty()) {
        loc = locs.top();
        locs.pop();
        ptr = loc->load();
        node = get_node(ptr);
        prev_loc = &node->prev;
        prev_ptr = prev_loc->load();
        always_assert(prev_ptr == nullptr || is_moved_or_locked(prev_ptr));
        auto new_hash = hasher()(const_key_projection()(node->value));
        auto* new_loc = &new_storage->ptrs[new_hash % new_storage->size];
        auto* new_ptr = new_loc->load();
        // Rewiring the node happens in three steps:
        do {
          // Assume there is no race with an erasure.
          new_ptr = get_node(new_ptr);
          // 1. Set the (null) prev node pointer to the first chain element in
          // the new storage version. This is ultimately what we want it to be;
          // it might allow a racing read operation to scan irrelevant nodes,
          // but that is not a problem for correctness.
          prev_loc->store(new_ptr);
          // 2. Wire up the current node pointer to be the first chain element
          // in the new storage version. This may fail due to a race with
          // another thread inserting into or erasing from the same chain. But
          // then we'll just retry.
        } while (!new_loc->compare_exchange_strong(new_ptr, node));
        // 3. Detach the current node pointer from the end of the old chain.
        loc->store(moved());
      }
    }
    auto* old_storage = m_storage.exchange(new_storage);
    always_assert(old_storage == storage);
    m_resizing.store(false);
    return true;
  }

  /*
   * This operation is always thread-safe.
   */
  value_type* erase(const key_type& key) {
    auto hash = hasher()(key);
    auto* storage = m_storage.load();
    while (true) {
      auto* ptrs = storage->ptrs;
      auto* root_loc = &ptrs[hash % storage->size];
      auto* root = root_loc->load();
      if (root == nullptr) {
        return nullptr;
      }
      if (root == moved()) {
        storage = storage->next.load();
        continue;
      }
      // The chain is not empty. Try to lock the bucket. This might fail due
      // to a race with an insertion, erasure, or resizing.
      auto* node = get_node(root);
      always_assert(node);
      root = node;
      if (!root_loc->compare_exchange_strong(root, lock(node))) {
        continue;
      }
      auto* loc = root_loc;
      for (; node && !key_equal()(const_key_projection()(node->value), key);
           loc = &node->prev, node = get_node(loc->load())) {
      }
      if (node) {
        // Erase node.
        loc->store(node->prev.load());
        m_count.fetch_sub(1);
        // Store erased node for later actual deletion.
        auto* erased = new Erased{node, nullptr};
        while (!m_erased.compare_exchange_strong(erased->prev, erased)) {
        }
      }
      if (loc != root_loc) {
        // Unlock root node (as we didn't erase it).
        root_loc->store(root);
      }
      return node ? &node->value : nullptr;
    }
  }

  /*
   * This operation is NOT thread-safe.
   */
  void compact() {
    process_erased();
    auto* storage = m_storage.load();
    always_assert(storage->next.load() == nullptr);
    Storage* prev_storage = nullptr;
    std::swap(storage->prev, prev_storage);
    Storage::destroy(prev_storage);
  }

 private:
  static constexpr float LOAD_FACTOR = 0.75;
  static constexpr size_t INITIAL_SIZE = 5;

  // We store Node pointers as tagged values, to indicate, and be able to
  // atomically update, whether a location where a node pointer is stored is
  // currently involved in an erasure or resizing operation.
  using Ptr = void*;
  static const size_t MOVED_OR_LOCKED = 1;
  using PtrPlusBits = boost::intrusive::pointer_plus_bits<Ptr, 1>;

  struct ConstRefValueTag {};
  struct RvalueRefValueTag {};
  struct ConstRefKeyArgsTag {};
  struct RvalueRefKeyArgsTag {};

  struct Node {
    value_type value;
    std::atomic<Ptr> prev{nullptr};

    explicit Node(ConstRefValueTag, const value_type& value) : value(value) {}

    explicit Node(RvalueRefValueTag, value_type&& value)
        : value(std::forward<value_type>(value)) {}

    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  std::is_same_v<key_type2, value_type>>>
    explicit Node(ConstRefKeyArgsTag, const key_type2& key) : value(key) {}

    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  std::is_same_v<key_type2, value_type>>>
    explicit Node(RvalueRefKeyArgsTag, key_type2&& key)
        : value(std::forward<key_type2>(key)) {}

    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  !std::is_same_v<key_type2, value_type>>,
              typename... Args>
    explicit Node(ConstRefKeyArgsTag, const key_type2& key, Args&&... args)
        : value(std::piecewise_construct,
                std::forward_as_tuple(key),
                std::forward_as_tuple(std::forward<Args>(args)...)) {}

    template <typename key_type2 = key_type,
              typename = typename std::enable_if_t<
                  !std::is_same_v<key_type2, value_type>>,
              typename... Args>
    explicit Node(RvalueRefKeyArgsTag, key_type2&& key, Args&&... args)
        : value(std::piecewise_construct,
                std::forward_as_tuple(std::forward<key_type2>(key)),
                std::forward_as_tuple(std::forward<Args>(args)...)) {}
  };

  // Initially, and every time we resize, a new Storage version is created.
  struct Storage {
    size_t size;
    Storage* prev;
    std::atomic<Storage*> next;
    std::atomic<Ptr> ptrs[1];

    // Only create instances via `create`.
    Storage() = delete;

    static Storage* create(size_t size, Storage* prev) {
      always_assert(size > 0);
      size_t bytes = sizeof(Storage) + sizeof(std::atomic<Ptr>) * (size - 1);
      always_assert(bytes % sizeof(size_t) == 0);
      auto* storage = (Storage*)calloc(bytes / sizeof(size_t), sizeof(size_t));
      always_assert(storage);
      always_assert(storage->prev == nullptr);
      storage->size = size;
      storage->prev = prev;
      return storage;
    }

    static Storage* create() { return create(INITIAL_SIZE, nullptr); }

    static void destroy(Storage* t) {
      for (auto* s = t; s; s = t) {
        if (s->next.load() == nullptr) {
          for (size_t i = 0; i < s->size; i++) {
            auto* loc = &s->ptrs[i];
            auto* ptr = loc->load();
            for (auto* node = get_node(ptr); node; node = get_node(ptr)) {
              ptr = node->prev.load();
              delete node;
            }
          }
        }
        t = s->prev;
        free(s);
      }
    }
  };

  std::atomic<Storage*> m_storage;
  std::atomic<size_t> m_count;
  std::atomic<bool> m_resizing{false};
  struct Erased {
    Node* node;
    Erased* prev;
  };
  std::atomic<Erased*> m_erased;

  bool load_factor_exceeded(const Storage* storage) const {
    return m_count.load() > storage->size * LOAD_FACTOR;
  }

  // Whether more elements can be found in the next Storage version, or if an
  // erasure is ongoing.
  static bool is_moved_or_locked(Ptr ptr) {
    return PtrPlusBits::get_bits(ptr) != 0;
  }

  static Node* get_node(Ptr ptr) {
    return static_cast<Node*>(PtrPlusBits::get_pointer(ptr));
  }

  // Creates a tagged `Ptr` indicating that this is a sentinel due to resizing;
  // if so, additional nodes may be found in the next Storage version.
  static Ptr moved() {
    Ptr ptr = nullptr;
    PtrPlusBits::set_bits(ptr, MOVED_OR_LOCKED);
    return ptr;
  }

  // Creates a tagged root node indicating that the node chain is in the process
  // of being resized or part of it is being erased. If there is a next Storage
  // version, any additional nodes must go to it.
  static Ptr lock(Node* node) {
    always_assert(node);
    Ptr ptr = node;
    PtrPlusBits::set_bits(ptr, MOVED_OR_LOCKED);
    return ptr;
  }

  // Creates a tagged `Ptr` either indicating that the bucket moved if the node
  // is absent, or locking the given node.
  static Ptr moved_or_lock(Node* node) {
    Ptr ptr = node;
    PtrPlusBits::set_bits(ptr, MOVED_OR_LOCKED);
    return ptr;
  }

  void process_erased() {
    for (auto* erased = m_erased.load(); erased != nullptr;) {
      delete erased->node;
      auto* prev = erased->prev;
      delete erased;
      erased = prev;
    }
    m_erased.store(nullptr);
  }

  friend class ConcurrentHashtableIterator<
      ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;
  friend class ConcurrentHashtableIterator<
      const ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;
  friend class ConcurrentHashtableInsertionResult<
      ConcurrentHashtable<Key, Value, Hash, KeyEqual>>;
};

/*
 * Helper class to represent result of an (attmpted) insertion. What's
 * interesting is that even when insertion fails, because a value with the same
 * key is already present in the hashtable, a new value might have been
 * incidentally constructed, possibly moving the supplied arguments in the
 * process. This result value captures such an incidentally created value, and
 * allows checking for equality with the stored value.
 */
template <typename ConcurrentHashtable>
class ConcurrentHashtableInsertionResult final {
  using value_type = typename ConcurrentHashtable::value_type;
  using Node = typename ConcurrentHashtable::Node;
  std::unique_ptr<Node> m_node;
  explicit ConcurrentHashtableInsertionResult(value_type* stored_value_ptr)
      : stored_value_ptr(stored_value_ptr), success(true) {}
  ConcurrentHashtableInsertionResult(value_type* stored_value_ptr, Node* node)
      : m_node(node), stored_value_ptr(stored_value_ptr), success(false) {}

 public:
  value_type* stored_value_ptr;
  bool success;

  value_type* incidentally_constructed_value() const {
    return m_node ? &m_node->value : nullptr;
  }

  friend ConcurrentHashtable;
};

template <typename ConcurrentHashtable>
class ConcurrentHashtableIterator final {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = typename ConcurrentHashtable::value_type;
  using pointer =
      std::conditional_t<std::is_const<ConcurrentHashtable>::value,
                         typename ConcurrentHashtable::const_pointer,
                         typename ConcurrentHashtable::pointer>;
  using const_pointer = typename ConcurrentHashtable::const_pointer;
  using reference =
      std::conditional_t<std::is_const<ConcurrentHashtable>::value,
                         typename ConcurrentHashtable::const_reference,
                         typename ConcurrentHashtable::reference>;
  using const_reference = typename ConcurrentHashtable::const_reference;
  using iterator_category = std::forward_iterator_tag;

 private:
  using Storage = typename ConcurrentHashtable::Storage;
  using Node = typename ConcurrentHashtable::Node;

  Storage* m_storage;
  size_t m_index;
  Node* m_node;

  bool is_end() const { return m_index == m_storage->size; }

  void advance() {
    if (m_node) {
      m_node = ConcurrentHashtable::get_node(m_node->prev.load());
      if (m_node) {
        return;
      }
    }
    do {
      if (++m_index == m_storage->size) {
        return;
      }
      m_node = ConcurrentHashtable::get_node(m_storage->ptrs[m_index].load());
    } while (!m_node);
  }

  ConcurrentHashtableIterator(Storage* storage, size_t index, Node* node)
      : m_storage(storage), m_index(index), m_node(node) {
    if (!node && index < storage->size) {
      advance();
    }
  }

 public:
  ConcurrentHashtableIterator& operator++() {
    always_assert(!is_end());
    advance();
    return *this;
  }

  ConcurrentHashtableIterator& operator++(int) {
    ConcurrentHashtableIterator retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(const ConcurrentHashtableIterator& other) const {
    return m_storage == other.m_storage && m_index == other.m_index &&
           m_node == other.m_node;
  }

  bool operator!=(const ConcurrentHashtableIterator& other) const {
    return !(*this == other);
  }

  reference operator*() {
    always_assert(!is_end());
    return m_node->value;
  }

  pointer operator->() {
    always_assert(!is_end());
    return &m_node->value;
  }

  const_reference operator*() const {
    always_assert(!is_end());
    return m_node->value;
  }

  const_pointer operator->() const {
    always_assert(!is_end());
    return &m_node->value;
  }

  friend ConcurrentHashtable;
};

} // namespace cc_impl

// Use this scope at the top-level function of your application to allow for
// fast concurrent destruction. Avoid changing the threshold in the global scope
// due to hard to control global destruction order, and our dependency on
// threading / the sparta-workqueue for concurrent destruction.
class ConcurrentContainerConcurrentDestructionScope {
  size_t m_last_threshold;

 public:
  explicit ConcurrentContainerConcurrentDestructionScope(
      size_t threshold = 4096)
      : m_last_threshold(threshold) {
    std::swap(cc_impl::s_concurrent_destruction_threshold, m_last_threshold);
  }

  ~ConcurrentContainerConcurrentDestructionScope() {
    std::swap(cc_impl::s_concurrent_destruction_threshold, m_last_threshold);
  }
};

/*
 * This class implements the common functionalities of concurrent sets and maps.
 * A concurrent container is a collection of a ConcurrentHashtable
 * (providing functionality similar yo unordered_map/unordered_set) arranged in
 * slots. Whenever a thread performs a concurrent operation on an element, the
 * slot is uniquely determined by the hash code of the element. A sharded lock
 * is obtained if the operation in question cannot be performed lock-free. A
 * high number of slots may help reduce thread contention at the expense of a
 * larger memory footprint. It is advised to use a prime number for `n_slots`,
 * so as to ensure a more even spread of elements across slots.
 *
 * There are two major modes in which a concurrent container is thread-safe:
 *  - Read only: multiple threads access the contents of the container but do
 *    not attempt to modify any element.
 *  - Write only: multiple threads update the contents of the container but do
 *    not otherwise attempt to access any element.
 * The few operations that are thread-safe regardless of the access mode are
 * documented as such.
 */
template <typename Container, size_t n_slots>
class ConcurrentContainer {
 public:
  static_assert(n_slots > 0, "The concurrent container has no slots");

  using Key = typename Container::key_type;
  using Hash = typename Container::hasher;
  using KeyEqual = typename Container::key_equal;
  using Value = typename Container::value_type;

  using ConcurrentHashtable =
      cc_impl::ConcurrentHashtable<Key, Value, Hash, KeyEqual>;

  using iterator =
      cc_impl::ConcurrentContainerIterator<ConcurrentHashtable, n_slots>;

  using const_iterator =
      cc_impl::ConcurrentContainerIterator<const ConcurrentHashtable, n_slots>;

  virtual ~ConcurrentContainer() {
    auto timer_scope = cc_impl::s_destructor.scope();
    if (!cc_impl::is_thread_pool_active() ||
        size() <= cc_impl::s_concurrent_destruction_threshold) {
      for (size_t slot = 0; slot < n_slots; ++slot) {
        m_slots[slot].destroy();
      }
      return;
    }
    cc_impl::workqueue_run_for(
        0, n_slots, [this](size_t slot) { m_slots[slot].destroy(); });
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

  /*
   * This operation is always thread-safe.
   */
  size_t size() const {
    size_t s = 0;
    for (size_t slot = 0; slot < n_slots; ++slot) {
      s += m_slots[slot].size();
    }
    return s;
  }

  /*
   * This operation is always thread-safe.
   */
  bool empty() const {
    for (size_t slot = 0; slot < n_slots; ++slot) {
      if (!m_slots[slot].empty()) {
        return false;
      }
    }
    return true;
  }

  /*
   * This operation is always thread-safe.
   */
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

  void compact() {
    for (size_t slot = 0; slot < n_slots; ++slot) {
      m_slots[slot].compact();
    }
  }

  /*
   * This operation is always thread-safe.
   */
  size_t count(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    return m_slots[slot].get(key) != nullptr;
  }

  size_t count_unsafe(const Key& key) const { return count(key); }

  /*
   * This operation is always thread-safe.
   */
  size_t erase(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    return m_slots[slot].erase(key) ? 1 : 0;
  }

  size_t erase_unsafe(const Key& key) { return erase(key); }

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

  ConcurrentContainer& operator=(ConcurrentContainer&& container) noexcept {
    for (size_t i = 0; i < n_slots; ++i) {
      m_slots[i] = std::move(container.m_slots[i]);
    }
    return *this;
  }

  ConcurrentContainer& operator=(
      const ConcurrentContainer& container) noexcept {
    if (this != &container) {
      for (size_t i = 0; i < n_slots; ++i) {
        m_slots[i] = container.m_slots[i];
      }
    }
    return *this;
  }

  ConcurrentHashtable& get_container(size_t slot) { return m_slots[slot]; }

  const ConcurrentHashtable& get_container(size_t slot) const {
    return m_slots[slot];
  }

  ConcurrentHashtable m_slots[n_slots];
};

/*
 * A concurrent container with map semantics, also allowing erasing and updating
 * values.
 *
 * Compared to other concurrent datatypes, the ConcurrentMap has additional
 * overhead: It maintains another set mutexes, one per slot, and even reading a
 * value safely requires aquiring a lock.
 *
 * Prefer using an InsertOnlyConcurrentMap or an AtomicMap when possibly:
 * - InsertOnlyConcurrentMap more clearly conveys the possible intent of an
 * insertion-only map whose elements cannot be mutated, and it allows safely
 * reading values without requiring copying them under a lock.
 * - AtomicMap typically allows for lock free reads and writes, and even
 * supports all common lock-free atomic mutations typically found on
 * std::atomic<>.
 */
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          size_t n_slots = cc_impl::kDefaultSlots>
class ConcurrentMap final
    : public ConcurrentContainer<std::unordered_map<Key, Value, Hash, KeyEqual>,
                                 n_slots> {
 public:
  using Base =
      ConcurrentContainer<std::unordered_map<Key, Value, Hash, KeyEqual>,
                          n_slots>;

  using typename Base::const_iterator;
  using typename Base::iterator;

  using Base::end;
  using Base::m_slots;

  using KeyValuePair = typename Base::Value;

  ConcurrentMap() = default;

  ConcurrentMap(const ConcurrentMap& container) noexcept : Base(container) {}

  ConcurrentMap(ConcurrentMap&& container) noexcept
      : Base(std::move(container)) {}

  ConcurrentMap& operator=(ConcurrentMap&& container) noexcept {
    Base::operator=(std::move(container));
    return *this;
  }

  ConcurrentMap& operator=(const ConcurrentMap& container) noexcept {
    if (this != &container) {
      Base::operator=(container);
    }
    return *this;
  }

  template <typename InputIt>
  ConcurrentMap(InputIt first, InputIt last) {
    insert(first, last);
  }

  /*
   * This operation is always thread-safe. Note that it returns a copy of Value
   * rather than a reference since erasing or updating from other threads may
   * cause a stored value to become invalid. If you are reading from a
   * ConcurrentMap that is not being concurrently modified, it will probably be
   * faster to use `find()` or `at_unsafe()` to avoid the copy.
   */
  Value at(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    if (ptr == nullptr) {
      throw std::out_of_range("at");
    }
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    return ptr->second;
  }

  const Value& at_unsafe(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    if (ptr == nullptr) {
      throw std::out_of_range("at_unsafe");
    }
    return ptr->second;
  }

  Value& at_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto* ptr = map.get(key);
    if (ptr == nullptr) {
      throw std::out_of_range("at_unsafe");
    }
    return ptr->second;
  }

  /*
   * This operation is always thread-safe.
   */
  Value get(const Key& key, Value default_value) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    if (!ptr) {
      return default_value;
    }
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    return ptr->second;
  }

  Value* get_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto* ptr = map.get(key);
    return ptr ? &ptr->second : nullptr;
  }

  const Value* get_unsafe(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    return ptr ? &ptr->second : nullptr;
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
  bool insert(const KeyValuePair& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    return map.try_insert(entry).success;
  }

  bool insert(KeyValuePair&& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    return map.try_insert(std::forward<KeyValuePair>(entry)).success;
  }

  /*
   * This operation is always thread-safe.
   */
  void insert(std::initializer_list<KeyValuePair> l) {
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
  void insert_or_assign(const KeyValuePair& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(entry);
    if (insertion_result.success) {
      return;
    }
    auto* constructed_value = insertion_result.incidentally_constructed_value();
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    if (constructed_value) {
      insertion_result.stored_value_ptr->second =
          std::move(constructed_value->second);
    } else {
      insertion_result.stored_value_ptr->second = entry.second;
    }
  }

  void insert_or_assign(KeyValuePair&& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(std::forward<KeyValuePair>(entry));
    if (insertion_result.success) {
      return;
    }
    auto* constructed_value = insertion_result.incidentally_constructed_value();
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    if (constructed_value) {
      insertion_result.stored_value_ptr->second =
          std::move(constructed_value->second);
    } else {
      insertion_result.stored_value_ptr->second =
          std::forward<Value>(entry.second);
    }
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  bool emplace(const Key& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    return map.try_emplace(key, std::forward<Args>(args)...).success;
  }

  template <typename... Args>
  bool emplace(Key&& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    return map.try_emplace(std::forward<Key>(key), std::forward<Args>(args)...)
        .success;
  }

  template <typename... Args>
  std::pair<Value*, bool> emplace_unsafe(const Key& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, std::forward<Args>(args)...);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  template <typename... Args>
  std::pair<Value*, bool> emplace_unsafe(Key&& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result =
        map.try_emplace(std::forward<Key>(key), std::forward<Args>(args)...);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  /*
   * This operation atomically observes an entry in the map if it exists.
   */
  template <
      typename ObserveFn = const std::function<void(const Key&, const Value&)>&>
  bool observe(const Key& key, ObserveFn observer) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    if (!ptr) {
      return false;
    }
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    observer(ptr->first, ptr->second);
    return true;
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
    auto& map = this->get_container(slot);
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    auto insertion_result = map.try_emplace(key);
    auto* ptr = insertion_result.stored_value_ptr;
    updater(ptr->first, ptr->second, !insertion_result.success);
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update(Key&& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    std::unique_lock<std::mutex> lock(this->get_lock_by_slot(slot));
    auto insertion_result = map.try_emplace(std::forward<Key>(key));
    auto* ptr = insertion_result.stored_value_ptr;
    updater(ptr->first, ptr->second, !insertion_result.success);
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update_unsafe(const Key& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key);
    auto* ptr = insertion_result.stored_value_ptr;
    updater(ptr->first, ptr->second, !insertion_result.success);
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update_unsafe(Key&& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(std::forward<Key>(key));
    auto* ptr = insertion_result.stored_value_ptr;
    updater(ptr->first, ptr->second, !insertion_result.success);
  }

  /*
   * This operation is always thread-safe. If the key exists, a non-null pointer
   * to the corresponding value is returned. This pointer is valid until this
   * container is destroyed, or the compact function is called. Note that there
   * may be other threads that are also reading or even updating the value. If
   * the key is added to the map again, its new value would be stored in a new
   * independent location.
   */
  Value* get_and_erase(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto ptr = m_slots[slot].erase(key);
    return ptr ? &ptr->second : nullptr;
  }

 private:
  std::mutex& get_lock_by_slot(size_t slot) const { return m_locks[slot]; }

  mutable std::mutex m_locks[n_slots];
};

/**
 * A concurrent container with map semantics that only accepts insertions.
 *
 * This allows accessing constant references on values safely and lock-free.
 */
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          size_t n_slots = cc_impl::kDefaultSlots>
class InsertOnlyConcurrentMap final
    : public ConcurrentContainer<std::unordered_map<Key, Value, Hash, KeyEqual>,
                                 n_slots> {
 public:
  using Base =
      ConcurrentContainer<std::unordered_map<Key, Value, Hash, KeyEqual>,
                          n_slots>;
  using typename Base::const_iterator;
  using typename Base::iterator;

  using Base::end;
  using Base::m_slots;

  using KeyValuePair = typename Base::Value;

  InsertOnlyConcurrentMap() = default;

  InsertOnlyConcurrentMap(const InsertOnlyConcurrentMap& container) noexcept
      : Base(container) {}

  InsertOnlyConcurrentMap(InsertOnlyConcurrentMap&& container) noexcept
      : Base(std::move(container)) {}

  InsertOnlyConcurrentMap& operator=(
      InsertOnlyConcurrentMap&& container) noexcept {
    Base::operator=(std::move(container));
    return *this;
  }

  InsertOnlyConcurrentMap& operator=(
      const InsertOnlyConcurrentMap& container) noexcept {
    if (this != &container) {
      Base::operator=(container);
    }
    return *this;
  }

  template <typename InputIt>
  InsertOnlyConcurrentMap(InputIt first, InputIt last) {
    insert(first, last);
  }

  /*
   * This operation is always thread-safe.
   */
  const Value* get(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    return ptr ? &ptr->second : nullptr;
  }

  Value* get_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto* ptr = map.get(key);
    return ptr ? &ptr->second : nullptr;
  }

  const Value* get_unsafe(const Key& key) const { return get(key); }

  /*
   * This operation is always thread-safe. If you are reading from a
   * ConcurrentMap that is not being concurrently modified, it will probably be
   * faster to use `find()` or `at_unsafe()` to avoid locking.
   */
  const Value& at(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    if (ptr == nullptr) {
      throw std::out_of_range("at");
    }
    return ptr->second;
  }

  Value& at_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto* ptr = map.get(key);
    if (ptr == nullptr) {
      throw std::out_of_range("at_unsafe");
    }
    return ptr->second;
  }

  const Value& at_unsafe(const Key& key) const { return at(key); }

  /*
   * Returns a pair consisting of a pointer on the inserted element (or the
   * element that prevented the insertion) and a boolean denoting whether the
   * insertion took place. This operation is always thread-safe.
   */
  std::pair<const Value*, bool> insert(const KeyValuePair& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(entry);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  /*
   * Returns a pair consisting of a pointer on the inserted element (or the
   * element that prevented the insertion) and a boolean denoting whether the
   * insertion took place. This operation is always thread-safe.
   */
  std::pair<const Value*, bool> insert(KeyValuePair&& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(std::move(entry));
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  std::pair<Value*, bool> insert_unsafe(const KeyValuePair& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(entry);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  std::pair<Value*, bool> insert_unsafe(KeyValuePair entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(std::move(entry));
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  /*
   * This operation is always thread-safe.
   */
  void insert(std::initializer_list<KeyValuePair> l) {
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

  std::pair<Value*, bool> insert_or_assign_unsafe(KeyValuePair&& entry) {
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(std::forward<KeyValuePair>(entry));
    if (insertion_result.success) {
      return std::make_pair(&insertion_result.stored_value_ptr->second, true);
    }
    auto* constructed_value = insertion_result.incidentally_constructed_value();
    if (constructed_value) {
      insertion_result.stored_value_ptr->second =
          std::move(constructed_value->second);
    } else {
      insertion_result.stored_value_ptr->second =
          std::forward<Value>(entry.second);
    }
    return std::make_pair(&insertion_result.stored_value_ptr->second, false);
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  std::pair<const Value*, bool> emplace(Args&&... args) {
    KeyValuePair entry(std::forward<Args>(args)...);
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(std::move(entry));
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  template <typename... Args>
  std::pair<Value*, bool> emplace_unsafe(Args&&... args) {
    KeyValuePair entry(std::forward<Args>(args)...);
    size_t slot = Hash()(entry.first) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(std::move(entry));
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename ValueEqual = std::equal_to<Value>, typename... Args>
  std::pair<const Value*, bool> get_or_emplace_and_assert_equal(
      Key&& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result =
        map.try_emplace(std::forward<Key>(key), std::forward<Args>(args)...);
    always_assert(
        insertion_result.success ||
        (insertion_result.incidentally_constructed_value()
             ? ValueEqual()(
                   insertion_result.stored_value_ptr->second,
                   insertion_result.incidentally_constructed_value()->second)
             : ValueEqual()(insertion_result.stored_value_ptr->second,
                            Value(std::forward<Args>(args)...))));
    auto* ptr = insertion_result.stored_value_ptr;
    return {&ptr->second, insertion_result.success};
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename ValueEqual = std::equal_to<Value>, typename... Args>
  std::pair<const Value*, bool> get_or_emplace_and_assert_equal(
      const Key& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, std::forward<Args>(args)...);
    always_assert(
        insertion_result.success ||
        (insertion_result.incidentally_constructed_value()
             ? ValueEqual()(
                   insertion_result.stored_value_ptr->second,
                   insertion_result.incidentally_constructed_value()->second)
             : ValueEqual()(insertion_result.stored_value_ptr->second,
                            Value(std::forward<Args>(args)...))));
    auto* ptr = insertion_result.stored_value_ptr;
    return {&ptr->second, insertion_result.success};
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename ValueEqual = std::equal_to<Value>,
            typename Creator,
            typename... Args>
  std::pair<const Value*, bool> get_or_create_and_assert_equal(
      const Key& key, const Creator& creator, Args&&... args) {
    auto* ptr = get(key);
    if (ptr) {
      return {ptr, false};
    }
    return get_or_emplace_and_assert_equal<ValueEqual>(
        key, creator(key, std::forward<Args>(args)...));
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename ValueEqual = std::equal_to<Value>,
            typename Creator,
            typename... Args>
  std::pair<const Value*, bool> get_or_create_and_assert_equal(
      Key&& key, const Creator& creator, Args&&... args) {
    auto* ptr = get(key);
    if (ptr) {
      return {ptr, false};
    }
    return get_or_emplace_and_assert_equal<ValueEqual>(
        std::forward<Key>(key),
        creator(std::forward<Key>(key), std::forward<Args>(args)...));
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update_unsafe(const Key& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto [ptr, emplaced] = map.try_emplace(key);
    updater(ptr->first, ptr->second, !emplaced);
  }

  template <
      typename UpdateFn = const std::function<void(const Key&, Value&, bool)>&>
  void update_unsafe(Key&& key, UpdateFn updater) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto [ptr, emplaced] = map.try_emplace(std::forward<Key>(key));
    updater(ptr->first, ptr->second, !emplaced);
  }

  size_t erase(const Key& key) = delete;
};

/**
 * A concurrent container with map semantics that holds atomic values.
 *
 * This typically allows for lock free reads and writes (if the underlying
 * std::atomic<> data types allows), and even supports all common lock-free
 * atomic mutations typically found on std::atomic<>.
 */
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          size_t n_slots = cc_impl::kDefaultSlots>
class AtomicMap final
    : public ConcurrentContainer<
          std::unordered_map<Key, std::atomic<Value>, Hash, KeyEqual>,
          n_slots> {
 public:
  using Base = ConcurrentContainer<
      std::unordered_map<Key, std::atomic<Value>, Hash, KeyEqual>,
      n_slots>;
  using typename Base::const_iterator;
  using typename Base::iterator;

  using Base::end;
  using Base::m_slots;

  AtomicMap() = default;

  AtomicMap(const AtomicMap& container) noexcept : Base(container) {}

  AtomicMap(AtomicMap&& container) noexcept : Base(std::move(container)) {}

  AtomicMap& operator=(AtomicMap&& container) noexcept {
    Base::operator=(std::move(container));
    return *this;
  }

  AtomicMap& operator=(const AtomicMap& container) noexcept {
    if (this != &container) {
      Base::operator=(container);
    }
    return *this;
  }

  /*
   * This operation is always thread-safe.
   */
  Value load(const Key& key, Value default_value = Value()) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& map = this->get_container(slot);
    const auto* ptr = map.get(key);
    return ptr ? ptr->second.load() : default_value;
  }

  /*
   * This operation is always thread-safe.
   */
  std::atomic<Value>* store(const Key& key, const Value& arg) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, arg);
    if (!insertion_result.success) {
      insertion_result.stored_value_ptr->second.store(arg);
    }
    return &insertion_result.stored_value_ptr->second;
  }

  /*
   * This operation is always thread-safe.
   */
  std::atomic<Value>* store(Key&& key, Value&& arg) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result =
        map.try_emplace(std::forward<Key>(key), std::forward<Value>(arg));
    if (!insertion_result->success) {
      auto* constructed_value =
          insertion_result.incidentally_constructed_value();
      if (constructed_value) {
        insertion_result.stored_value_ptr->second.store(
            std::move(constructed_value->second));
      } else {
        insertion_result.stored_value_ptr->second = std::forward<Value>(arg);
      }
    }
    return &insertion_result.stored_value_ptr->second;
  }

  /*
   * This operation is always thread-safe.
   */
  template <typename... Args>
  std::pair<std::atomic<Value>*, bool> emplace(const Key& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, std::forward<Args>(args)...);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  template <typename... Args>
  std::pair<std::atomic<Value>*, bool> emplace(Key&& key, Args&&... args) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result =
        map.try_emplace(std::forward<Key>(key), std::forward<Args>(args)...);
    return std::make_pair(&insertion_result.stored_value_ptr->second,
                          insertion_result.success);
  }

  /*
   * This operation is always thread-safe.
   */
  Value exchange(const Key& key, Value desired, Value default_value = Value()) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, desired);
    if (insertion_result.success) {
      return default_value;
    }
    return insertion_result.stored_value_ptr->second.exchange(desired);
  }

  /*
   * This operation is always thread-safe.
   */
  bool compare_exchange(const Key& key,
                        Value& expected,
                        Value desired,
                        Value default_value = Value()) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    if (expected == default_value) {
      auto insertion_result = map.try_emplace(key, desired);
      if (insertion_result.success) {
        return true;
      }
      return insertion_result.stored_value_ptr->second.compare_exchange_strong(
          expected, desired);
    }
    auto ptr = map.get(key);
    if (ptr == nullptr) {
      expected = default_value;
      return false;
    }
    return ptr->second.compare_exchange_strong(expected, desired);
  }

  /*
   * This operation is always thread-safe.
   */
  Value fetch_add(const Key& key, Value arg, Value default_value = 0) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_emplace(key, default_value);
    return insertion_result.stored_value_ptr->second.fetch_add(arg);
  }

  /*
   * This operation is always thread-safe.
   */
  Value fetch_sub(const Key& key, Value arg, Value default_value = 0) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(key, default_value);
    return insertion_result.stored_value_ptr->second.fetch_sub(arg);
  }

  /*
   * This operation is always thread-safe.
   */
  Value fetch_and(const Key& key, Value arg, Value default_value = 0) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(key, default_value);
    return insertion_result.stored_value_ptr->second.fetch_and(arg);
  }

  /*
   * This operation is always thread-safe.
   */
  Value fetch_or(const Key& key, Value arg, Value default_value = 0) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(key, default_value);
    return insertion_result.stored_value_ptr->second.fetch_or(arg);
  }

  /*
   * This operation is always thread-safe.
   */
  Value fetch_xor(const Key& key, Value arg, Value default_value = 0) {
    size_t slot = Hash()(key) % n_slots;
    auto& map = this->get_container(slot);
    auto insertion_result = map.try_insert(key, default_value);
    return insertion_result.stored_value_ptr->second.fetch_xor(arg);
  }
};

/*
 * A concurrent container with set semantics, also allowing erasing values.
 */
template <typename Key,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          size_t n_slots = cc_impl::kDefaultSlots>
class ConcurrentSet final
    : public ConcurrentContainer<std::unordered_set<Key, Hash, KeyEqual>,
                                 n_slots> {
 public:
  using Base =
      ConcurrentContainer<std::unordered_set<Key, Hash, KeyEqual>, n_slots>;

  ConcurrentSet() = default;

  ConcurrentSet(const ConcurrentSet& set) noexcept : Base(set) {}

  ConcurrentSet(ConcurrentSet&& set) noexcept : Base(std::move(set)) {}

  ConcurrentSet& operator=(ConcurrentSet&& container) noexcept {
    Base::operator=(std::move(container));
    return *this;
  }

  ConcurrentSet& operator=(const ConcurrentSet& container) noexcept {
    if (this != &container) {
      Base::operator=(container);
    }
    return *this;
  }

  /*
   * The Boolean return value denotes whether the insertion took place.
   * This operation is always thread-safe.
   */
  bool insert(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    return set.try_insert(key).success;
  }

  bool insert(Key&& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    return set.try_insert(std::forward<Key>(key)).success;
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
    auto& set = this->get_container(slot);
    return set.try_insert(std::move(key)).success;
  }
};

/**
// A concurrent container with set semantics that only accepts insertions.
 *
 * This allows accessing constant references on elements safely.
 */
template <typename Key,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          size_t n_slots = cc_impl::kDefaultSlots>
class InsertOnlyConcurrentSet final
    : public ConcurrentContainer<std::unordered_set<Key, Hash, KeyEqual>,
                                 n_slots> {
 public:
  using Base =
      ConcurrentContainer<std::unordered_set<Key, Hash, KeyEqual>, n_slots>;

  InsertOnlyConcurrentSet() = default;

  InsertOnlyConcurrentSet(const InsertOnlyConcurrentSet& set) noexcept
      : Base(set) {}

  InsertOnlyConcurrentSet(InsertOnlyConcurrentSet&& set) noexcept
      : Base(std::move(set)) {}

  InsertOnlyConcurrentSet& operator=(
      InsertOnlyConcurrentSet&& container) noexcept {
    Base::operator=(std::move(container));
    return *this;
  }

  InsertOnlyConcurrentSet& operator=(
      const InsertOnlyConcurrentSet& container) noexcept {
    if (this != &container) {
      Base::operator=(container);
    }
    return *this;
  }

  /*
   * Returns a pair consisting of a pointer on the inserted element (or the
   * element that prevented the insertion) and a boolean denoting whether the
   * insertion took place. This operation is always thread-safe.
   */
  std::pair<const Key*, bool> insert(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    auto insertion_result = set.try_insert(key);
    return {insertion_result.stored_value_ptr, insertion_result.success};
  }

  /*
   * Returns a pair consisting of a pointer on the inserted element (or the
   * element that prevented the insertion) and a boolean denoting whether the
   * insertion took place. This operation is always thread-safe.
   */
  std::pair<const Key*, bool> insert(Key&& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    auto insertion_result = set.try_insert(std::forward<Key>(key));
    return {insertion_result.stored_value_ptr, insertion_result.success};
  }

  std::pair<Key*, bool> insert_unsafe(const Key& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    auto insertion_result = set.try_insert(key);
    return {insertion_result.stored_value_ptr, insertion_result.success};
  }

  std::pair<Key*, bool> insert_unsafe(Key&& key) {
    size_t slot = Hash()(key) % n_slots;
    auto& set = this->get_container(slot);
    auto insertion_result = set.try_insert(std::forward<Key>(key));
    return {insertion_result.stored_value_ptr, insertion_result.success};
  }

  /*
   * Return a pointer on the element, or `nullptr` if the element is not in the
   * set. This operation is always thread-safe.
   */
  const Key* get(const Key& key) const {
    size_t slot = Hash()(key) % n_slots;
    const auto& set = this->get_container(slot);
    return set.get(key);
  }

  size_t erase(const Key& key) = delete;
};

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
