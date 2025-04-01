/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This file defines zero-cost wrappers around std::unordered_map and
 * std::unordered_set that prevent accidental non-deterministic iteration.
 *
 * TODO: Define UnorderedMultiMap.
 * TODO: Replace (virtually) all usages of std::unordered_x with UnorderedX in
 * the codebase.
 *
 * Some std::unordered_map operations technically expose an iterator, but are
 * not typically used in a way that would require full iteration functionality.
 * The functions provided on the unordered collection instead return a fixed
 * iterator that doesn't allow stepping to other elements. This affects the
 * following functions:
 * - `insert()` (and its variants).
 * - `find()`
 * - `erase()`
 * - `end()`
 *
 * There are various global helper functions with the word "unordered" in their
 * names to make it very obvious that they involve operations on (potentially)
 * unordered collections. All uses of these functions should be closely reviewed
 * for potential order dependencies and thus source of non-determinism.
 * - `UnorderedIterable(collection)` exposes iterators (and associated mutation
 *   operations) of an unordered collection. In fact, `UnorderedIterable` can
 *   also forward any other (iterable) collection iterator. This is used by
 *   order- and collection-type-agnostic algorithms such as workqueue_run. (I am
 *   aware that Pascal-casing this function goes against our style guide, but
 *   it's nicely symmetric to common InstructionIterable constructor calls.)
 * - `unordered_any(collection)` is a short-hand version to select an arbitrary
 *   element of a (potentially) unordered collection.
 * - `unordered_accumulate(collection, ...)` provides the same functionality as
 *   `std::accumulate`, except that it works on (potentially) unordered
 *   collections.
 * - `unordered_copy(collection, ...)` / `unordered_copy_if(collection, ...)`
 *   provides the same functionality as `std::copy` / `std::copy_if`, except
 *   that it works on (potentially) unordered collections.
 * - `unordered_count(collection, ...)` / `unordered_count_if(collection, ...)`
 *   provides the same functionality as `std::count` / `std::count_if`, except
 *   that it works on (potentially) unordered collections.
 * - `unordered_erase_if(collection, ...)` provides the same functionality as
 *   `std::erase_if`, except that it works on (potentially) unordered
 *   collections.
 * - `transform(collection, ...)` provides the same functionality as
 *   `std::transform`, except that it works on (potentially) unordered
 *   collections.
 * - `unordered_all_of(collection, ...)` / `unordered_any_of` /
 *   `unordered_all_of` / `unordered_for_each` provide the same functionality as
 *   `std::all_of` / `std::any_of` / `std::none_of` / `std::for_each`, except
 *   that they work on (potentially) unordered collections.
 * - `insert_unordered_iterable(target, source)` allows inserting elements of a
 *   (potentially) unordered source into a target collection.
 *
 * A few additional global functions are provided that realize common operations
 * that use iterators, but don't ultimately rely on their order:
 * - `unordered_order_keys(collection, compare)`: Returns a vector of all keys,
 *   sorted.
 * - `unordered_order(collection, compare)`: Returns a vector of all key-value
 *   pairs, sorted by key.
 *
 * The global function `unordered_unsafe_unwrap(collection)` provides the
 * ultimate escape mechanism to expose the raw unordered collection, or simply
 * passes through any other collection. Use at your own risk.
 *
 * The new unordered collection wrapper types  doesn't provide wrappers for all
 * members yet; filling that in is a work-in-progress.
 *
 * TODO: An intention of the design is to largely keep open the option of
 * redefining UnorderedX as std::unordered_x, disabling all UnorderedX
 * specific function overloads, to get back the vanilla performance.
 */

#pragma once

#include <algorithm>
#include <numeric>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "TemplateUtil.h"

template <class UnorderedDerived>
class UnorderedBase {};

template <class Key,
          class Value,
          class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class UnorderedMap : UnorderedBase<UnorderedMap<Key, Value, Hash, KeyEqual>> {
  using Type = std::unordered_map<Key, Value, Hash, KeyEqual>;
  Type m_data;

 public:
  using key_type = typename Type::key_type;
  using mapped_type = typename Type::mapped_type;
  using value_type = typename Type::value_type;
  using size_type = typename Type::size_type;
  using difference_type = typename Type::difference_type;
  using hasher = typename Type::hasher;
  using key_equal = typename Type::key_equal;
  using reference = typename Type::reference;
  using const_reference = typename Type::const_reference;
  using pointer = typename Type::pointer;

  class FixedIterator;

  class ConstFixedIterator {
    typename Type::const_iterator m_entry;

   public:
    const value_type* operator->() const { return &*m_entry; }

    const value_type& operator*() const { return *m_entry; }

    bool operator==(const FixedIterator& other) const {
      return m_entry == other._internal_unsafe_unwrap();
    }

    bool operator!=(const FixedIterator& other) const {
      return m_entry != other._internal_unsafe_unwrap();
    }

    bool operator==(const ConstFixedIterator& other) const {
      return m_entry == other.m_entry;
    }

    bool operator!=(const ConstFixedIterator& other) const {
      return m_entry != other.m_entry;
    }

    explicit ConstFixedIterator(typename Type::const_iterator entry)
        : m_entry(entry) {}

    typename Type::const_iterator _internal_unsafe_unwrap() const {
      return m_entry;
    }
  };

  class FixedIterator {
    typename Type::iterator m_entry;

   public:
    value_type* operator->() const { return &*m_entry; }

    value_type& operator*() const { return *m_entry; }

    bool operator==(const FixedIterator& other) const {
      return m_entry == other.m_entry;
    }

    bool operator!=(const FixedIterator& other) const {
      return m_entry != other.m_entry;
    }

    bool operator==(const ConstFixedIterator& other) const {
      return m_entry == other._internal_unsafe_unwrap();
    }

    bool operator!=(const ConstFixedIterator& other) const {
      return m_entry != other._internal_unsafe_unwrap();
    }

    explicit FixedIterator(typename Type::iterator entry) : m_entry(entry) {}

    typename Type::iterator _internal_unsafe_unwrap() const { return m_entry; }
  };

  // TODO: Make extra non-deterministic in debug builds
  class UnorderedIterable {
    Type& m_data;

   public:
    using iterator = typename Type::iterator;
    using const_iterator = typename Type::const_iterator;

    explicit UnorderedIterable(Type& data) : m_data(data) {}

    iterator begin() { return m_data.begin(); }

    iterator end() { return m_data.end(); }

    const_iterator begin() const { return m_data.begin(); }

    const_iterator end() const { return m_data.end(); }

    const_iterator cbegin() const { return m_data.cbegin(); }

    const_iterator cend() const { return m_data.cend(); }

    iterator find(const Key& key) { return m_data.find(key); }

    const_iterator find(const Key& key) const { return m_data.find(key); }

    template <typename possibly_const_iterator>
    iterator erase(possibly_const_iterator position) {
      return m_data.erase(position);
    }
  };

  // TODO: Make extra non-deterministic in debug builds
  class ConstUnorderedIterable {
    const Type& m_data;

   public:
    using const_iterator = typename Type::const_iterator;

    explicit ConstUnorderedIterable(const Type& data) : m_data(data) {}

    const_iterator begin() const { return m_data.begin(); }

    const_iterator end() const { return m_data.end(); }

    const_iterator cbegin() const { return m_data.cbegin(); }

    const_iterator cend() const { return m_data.cend(); }

    const_iterator find(const Key& key) const { return m_data.find(key); }
  };

  UnorderedMap() : m_data() {}

  explicit UnorderedMap(size_t bucket_count) : m_data(bucket_count) {}

  UnorderedMap(size_t bucket_count, const Hash& hash)
      : m_data(bucket_count, hash) {}

  UnorderedMap(size_t bucket_count, const Hash& hash, const KeyEqual& equal)
      : m_data(bucket_count, hash, equal) {}

  UnorderedMap(const UnorderedMap& other) = default;

  UnorderedMap(UnorderedMap&& other) noexcept = default;

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  /* implicit */ UnorderedMap(
      std::initializer_list<std::pair<const Key, Value>> init)
      : m_data(init) {}

  template <class InputIt>
  UnorderedMap(InputIt first, InputIt last) : m_data(first, last) {}

  UnorderedMap& operator=(const UnorderedMap& other) = default;

  UnorderedMap& operator=(UnorderedMap&& other) noexcept = default;

  Value& at(const Key& key) { return m_data.at(key); }

  const Value& at(const Key& key) const { return m_data.at(key); }

  template <typename... Args>
  std::pair<FixedIterator, bool> emplace(Args&&... args) {
    auto [it, success] = m_data.emplace(std::forward<Args>(args)...);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  template <typename... Args>
  std::pair<FixedIterator, bool> try_emplace(Key&& k, Args&&... args) {
    auto [it, success] =
        m_data.try_emplace(std::forward<Key>(k), std::forward<Args>(args)...);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  template <typename... Args>
  std::pair<FixedIterator, bool> try_emplace(const Key& k, Args&&... args) {
    auto [it, success] = m_data.try_emplace(k, std::forward<Args>(args)...);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  std::pair<FixedIterator, bool> insert(const std::pair<Key, Value>& value) {
    auto [it, success] = m_data.insert(value);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  std::pair<FixedIterator, bool> insert(std::pair<Key, Value>&& value) {
    auto [it, success] =
        m_data.insert(std::forward<std::pair<Key, Value>>(value));
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  template <typename P>
  std::pair<FixedIterator, bool> insert(P&& value) {
    auto [it, success] = m_data.insert(std::forward<P>(value));
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  FixedIterator insert(ConstFixedIterator hint,
                       const std::pair<Key, Value>& value) {
    auto it = m_data.insert(hint._internal_unsafe_unwrap(), value);
    return FixedIterator(it);
  }

  std::pair<FixedIterator, bool> insert(ConstFixedIterator hint,
                                        std::pair<Key, Value>&& value) {
    auto it = m_data.insert(hint._internal_unsafe_unwrap(),
                            std::forward<std::pair<Key, Value>>(value));
    return FixedIterator(it);
  }

  template <typename P>
  std::pair<FixedIterator, bool> insert(ConstFixedIterator hint, P&& value) {
    auto it =
        m_data.insert(hint._internal_unsafe_unwrap(), std::forward<P>(value));
    return FixedIterator(it);
  }

  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    m_data.insert(first, last);
  }

  void insert(std::initializer_list<std::pair<Key, Value>> ilist) {
    m_data.insert(ilist);
  }

  Value& operator[](const Key& key) { return m_data[key]; }

  Value& operator[](Key&& key) { return m_data[std::forward<Key>(key)]; }

  FixedIterator _internal_unordered_any() {
    return FixedIterator(m_data.begin());
  }

  ConstFixedIterator _internal_unordered_any() const {
    return ConstFixedIterator(m_data.begin());
  }

  FixedIterator find(const Key& key) { return FixedIterator(m_data.find(key)); }

  ConstFixedIterator find(const Key& key) const {
    return ConstFixedIterator(m_data.find(key));
  }

  ConstFixedIterator end() const { return ConstFixedIterator(m_data.end()); }

  FixedIterator end() { return FixedIterator(m_data.end()); }

  ConstFixedIterator cend() const { return ConstFixedIterator(m_data.end()); }

  size_t erase(const Key& key) { return m_data.erase(key); }

  void erase(FixedIterator position) {
    // We intentionally do not return the iterator here, as it is not
    // ordered.
    (void)m_data.erase(position._internal_unsafe_unwrap());
  }

  void erase(ConstFixedIterator position) {
    // We intentionally do not return the iterator here, as it is not
    // ordered.
    m_data.erase(position._internal_unsafe_unwrap());
  }

  void clear() { m_data.clear(); }

  size_t count(const Key& key) const { return m_data.count(key); }

  void reserve(size_t size) { m_data.reserve(size); }

  size_t size() const { return m_data.size(); }

  bool empty() const { return m_data.empty(); }

  UnorderedIterable _internal_unordered_iterable() {
    return UnorderedIterable(m_data);
  }

  ConstUnorderedIterable _internal_unordered_iterable() const {
    return ConstUnorderedIterable(m_data);
  }

  const Type& _internal_unsafe_unwrap() const { return m_data; }

  Type& _internal_unsafe_unwrap() { return m_data; }
};

template <class Key, class Value, class Hash, class KeyEqual>
bool operator==(const UnorderedMap<Key, Value, Hash, KeyEqual>& lhs,
                const UnorderedMap<Key, Value, Hash, KeyEqual>& rhs) {
  return lhs._internal_unsafe_unwrap() == rhs._internal_unsafe_unwrap();
}

template <class Key, class Value, class Hash, class KeyEqual>
bool operator!=(const UnorderedMap<Key, Value, Hash, KeyEqual>& lhs,
                const UnorderedMap<Key, Value, Hash, KeyEqual>& rhs) {
  return lhs._internal_unsafe_unwrap() != rhs._internal_unsafe_unwrap();
}

template <class Key,
          class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class UnorderedSet : UnorderedBase<UnorderedSet<Key, Hash, KeyEqual>> {
  using Type = std::unordered_set<Key, Hash, KeyEqual>;
  Type m_data;

 public:
  using key_type = typename Type::key_type;
  using value_type = typename Type::value_type;
  using size_type = typename Type::size_type;
  using difference_type = typename Type::difference_type;
  using hasher = typename Type::hasher;
  using key_equal = typename Type::key_equal;
  using reference = typename Type::reference;
  using const_reference = typename Type::const_reference;
  using pointer = typename Type::pointer;

  class FixedIterator;

  class ConstFixedIterator {
    typename Type::const_iterator m_entry;

   public:
    const Key* operator->() const { return &*m_entry; }

    const Key& operator*() const { return *m_entry; }

    bool operator==(const FixedIterator& other) const {
      return m_entry == other._internal_unsafe_unwrap();
    }

    bool operator!=(const FixedIterator& other) const {
      return m_entry != other._internal_unsafe_unwrap();
    }

    bool operator==(const ConstFixedIterator& other) const {
      return m_entry == other.m_entry;
    }

    bool operator!=(const ConstFixedIterator& other) const {
      return m_entry != other.m_entry;
    }

    explicit ConstFixedIterator(typename Type::const_iterator entry)
        : m_entry(entry) {}

    typename Type::const_iterator _internal_unsafe_unwrap() const {
      return m_entry;
    }
  };

  class FixedIterator {
    typename Type::iterator m_entry;

   public:
    // Note that the Set iterator doesn't expose mutable values at all.

    const Key* operator->() const { return &*m_entry; }

    const Key& operator*() const { return *m_entry; }

    bool operator==(const FixedIterator& other) const {
      return m_entry == other.m_entry;
    }

    bool operator!=(const FixedIterator& other) const {
      return m_entry != other.m_entry;
    }

    bool operator==(const ConstFixedIterator& other) const {
      return m_entry == other._internal_unsafe_unwrap();
    }

    bool operator!=(const ConstFixedIterator& other) const {
      return m_entry != other._internal_unsafe_unwrap();
    }

    explicit FixedIterator(typename Type::iterator entry) : m_entry(entry) {}

    typename Type::iterator _internal_unsafe_unwrap() const { return m_entry; }
  };

  // TODO: Make extra non-deterministic in debug builds
  class UnorderedIterable {
    Type& m_data;

   public:
    using iterator = typename Type::iterator;
    using const_iterator = typename Type::const_iterator;

    explicit UnorderedIterable(Type& data) : m_data(data) {}

    iterator begin() { return m_data.begin(); }

    iterator end() { return m_data.end(); }

    const_iterator begin() const { return m_data.begin(); }

    const_iterator end() const { return m_data.end(); }

    const_iterator cbegin() const { return m_data.cbegin(); }

    const_iterator cend() const { return m_data.cend(); }

    iterator find(const Key& key) { return m_data.find(key); }

    const_iterator find(const Key& key) const { return m_data.find(key); }

    template <typename possibly_const_iterator>
    iterator erase(possibly_const_iterator position) {
      return m_data.erase(position);
    }
  };

  // TODO: Make extra non-deterministic in debug builds
  class ConstUnorderedIterable {
    const Type& m_data;

   public:
    using const_iterator = typename Type::const_iterator;

    explicit ConstUnorderedIterable(const Type& data) : m_data(data) {}

    const_iterator begin() const { return m_data.begin(); }

    const_iterator end() const { return m_data.end(); }

    const_iterator cbegin() const { return m_data.cbegin(); }

    const_iterator cend() const { return m_data.cend(); }

    const_iterator find(const Key& key) const { return m_data.find(key); }
  };

  UnorderedSet() : m_data() {}

  explicit UnorderedSet(size_t bucket_count) : m_data(bucket_count) {}

  UnorderedSet(size_t bucket_count, const Hash& hash)
      : m_data(bucket_count, hash) {}

  UnorderedSet(size_t bucket_count, const Hash& hash, const KeyEqual& equal)
      : m_data(bucket_count, hash, equal) {}

  UnorderedSet(const UnorderedSet& other) = default;

  UnorderedSet(UnorderedSet&& other) noexcept = default;

  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
  /* implicit */ UnorderedSet(std::initializer_list<Key> init) : m_data(init) {}

  template <class InputIt>
  UnorderedSet(InputIt first, InputIt last) : m_data(first, last) {}

  UnorderedSet& operator=(const UnorderedSet& other) = default;

  UnorderedSet& operator=(UnorderedSet&& other) noexcept = default;

  template <typename... Args>
  std::pair<FixedIterator, bool> emplace(Args&&... args) {
    auto [it, success] = m_data.emplace(std::forward<Args>(args)...);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  std::pair<FixedIterator, bool> insert(const Key& value) {
    auto [it, success] = m_data.insert(value);
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  std::pair<FixedIterator, bool> insert(Key&& value) {
    auto [it, success] = m_data.insert(std::forward<Key>(value));
    return std::pair<FixedIterator, bool>(FixedIterator(it), success);
  }

  FixedIterator insert(FixedIterator hint, const Key& value) {
    auto it = m_data.insert(hint._internal_unsafe_unwrap(), value);
    return FixedIterator(it);
  }

  FixedIterator insert(FixedIterator hint, Key&& value) {
    auto it =
        m_data.insert(hint._internal_unsafe_unwrap(), std::forward<Key>(value));
    return FixedIterator(it);
  }

  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    m_data.insert(first, last);
  }

  void insert(std::initializer_list<Key> ilist) { m_data.insert(ilist); }

  FixedIterator _internal_unordered_any() {
    return FixedIterator(m_data.begin());
  }

  ConstFixedIterator _internal_unordered_any() const {
    return ConstFixedIterator(m_data.begin());
  }

  FixedIterator find(const Key& key) { return FixedIterator(m_data.find(key)); }

  ConstFixedIterator find(const Key& key) const {
    return ConstFixedIterator(m_data.find(key));
  }

  ConstFixedIterator end() const { return ConstFixedIterator(m_data.end()); }

  FixedIterator end() { return FixedIterator(m_data.end()); }

  ConstFixedIterator cend() const { return ConstFixedIterator(m_data.end()); }

  size_t erase(const Key& key) { return m_data.erase(key); }

  void erase(FixedIterator position) {
    // We intentionally do not return the iterator here, as it is not
    // ordered.
    (void)m_data.erase(position._internal_unsafe_unwrap());
  }

  void erase(ConstFixedIterator position) {
    // We intentionally do not return the iterator here, as it is not
    // ordered.
    (void)m_data.erase(position._internal_unsafe_unwrap());
  }

  void clear() { m_data.clear(); }

  size_t count(const Key& key) const { return m_data.count(key); }

  void reserve(size_t size) { m_data.reserve(size); }

  size_t size() const { return m_data.size(); }

  bool empty() const { return m_data.empty(); }

  UnorderedIterable _internal_unordered_iterable() {
    return UnorderedIterable(m_data);
  }

  ConstUnorderedIterable _internal_unordered_iterable() const {
    return ConstUnorderedIterable(m_data);
  }

  const Type& _internal_unsafe_unwrap() const { return m_data; }

  Type& _internal_unsafe_unwrap() { return m_data; }
};

template <class Key, class Hash, class KeyEqual>
bool operator==(const UnorderedSet<Key, Hash, KeyEqual>& lhs,
                const UnorderedSet<Key, Hash, KeyEqual>& rhs) {
  return lhs._internal_unsafe_unwrap() == rhs._internal_unsafe_unwrap();
}

template <class Key, class Hash, class KeyEqual>
bool operator!=(const UnorderedSet<Key, Hash, KeyEqual>& lhs,
                const UnorderedSet<Key, Hash, KeyEqual>& rhs) {
  return lhs._internal_unsafe_unwrap() != rhs._internal_unsafe_unwrap();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
auto unordered_any(UnorderedCollection& collection) {
  return collection._internal_unordered_any();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
auto unordered_any(const UnorderedCollection& collection) {
  return collection._internal_unordered_any();
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
auto unordered_any(Collection& collection) {
  return collection.begin();
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
auto unordered_any(const Collection& collection) {
  return collection.begin();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
auto UnorderedIterable(UnorderedCollection& collection) {
  return collection._internal_unordered_iterable();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
auto UnorderedIterable(const UnorderedCollection& collection) {
  return collection._internal_unordered_iterable();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true,
          bool skip_assert = false>
auto UnorderedIterable(UnorderedCollection&& collection) {
  // This templated function get selected in an expression like the following:
  // for (auto x : UnorderedIterable(new UnorderedMap(...))) { ... }
  // However, the temporary collection may get destroyed before the end of the
  // loop. We want to avoid that, as the UnorderedIterable() only captures a
  // reference to the collection.
  static_assert(
      skip_assert,
      "Creating an UnorderedIterable from an rvalue is not implemented. Store "
      "the collection as temporary value in a variable instead, and ensure "
      "that the lifetime of the variable exceeds the lifetime of the "
      "iterable.");
  // We only leave this return-statement here so that a reasonable return type
  // can be inferred.
  return collection._internal_unordered_iterable();
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
Collection& UnorderedIterable(Collection& collection) {
  return collection;
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
const Collection& UnorderedIterable(const Collection& collection) {
  return collection;
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
const auto& unordered_unsafe_unwrap(const UnorderedCollection& collection) {
  return collection._internal_unsafe_unwrap();
}

template <class UnorderedCollection,
          std::enable_if_t<std::is_base_of_v<UnorderedBase<UnorderedCollection>,
                                             UnorderedCollection>,
                           bool> = true>
auto& unordered_unsafe_unwrap(UnorderedCollection& collection) {
  return collection._internal_unsafe_unwrap();
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
Collection& unordered_unsafe_unwrap(Collection& collection) {
  return collection;
}

template <
    class Collection,
    std::enable_if_t<!std::is_base_of_v<UnorderedBase<Collection>, Collection>,
                     bool> = true>
const Collection& unordered_unsafe_unwrap(const Collection& collection) {
  return collection;
}

template <
    class Collection,
    class Compare,
    class Key = typename std::remove_const<typename Collection::key_type>::type,
    class Value =
        typename std::remove_const<typename Collection::mapped_type>::type,
    std::enable_if_t<!std::is_same_v<typename Collection::key_type,
                                     typename Collection::value_type>,
                     bool> = true>
std::vector<std::pair<Key, Value>> unordered_order(Collection& collection,
                                                   Compare comp) {
  std::vector<std::pair<Key, Value>> result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.emplace_back(entry);
  }
  std::sort(result.begin(), result.end(), std::move(comp));
  return result;
}

template <class Collection,
          class Compare,
          class Value =
              typename std::remove_const<typename Collection::value_type>::type,
          std::enable_if_t<std::is_same_v<typename Collection::key_type,
                                          typename Collection::value_type>,
                           bool> = true>
std::vector<Value> unordered_order(Collection& collection, Compare comp) {
  std::vector<Value> result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.emplace_back(entry);
  }
  std::sort(result.begin(), result.end(), std::move(comp));
  return result;
}

template <
    class Collection,
    class Key = typename std::remove_const<typename Collection::key_type>::type>
std::vector<Key> unordered_order_keys(Collection& collection) {
  std::vector<Key> result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.emplace_back(entry.first);
  }
  std::sort(result.begin(), result.end());
  return result;
}

template <
    class Collection,
    class Compare,
    class Key = typename std::remove_const<typename Collection::key_type>::type>
std::vector<Key> unordered_order_keys(Collection& collection, Compare comp) {
  std::vector<Key> result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.emplace_back(entry.first);
  }
  std::sort(result.begin(), result.end(), std::move(comp));
  return result;
}

template <
    class Collection,
    class Key = typename std::remove_const<typename Collection::key_type>::type>
auto unordered_keys(Collection& collection) {
  UnorderedSet<Key, typename Collection::hasher, typename Collection::key_equal>
      result;
  result.reserve(collection.size());
  for (auto& entry : UnorderedIterable(collection)) {
    result.insert(entry.first);
  }
  return result;
}

template <class Collection, class T>
T unordered_accumulate(const Collection& collection, T init) {
  auto ui = UnorderedIterable(collection);
  return std::accumulate(ui.begin(), ui.end(), std::move(init));
}

template <class Collection, class T, class BinaryOp>
T unordered_accumulate(const Collection& collection, T init, BinaryOp op) {
  auto ui = UnorderedIterable(collection);
  return std::accumulate(ui.begin(), ui.end(), std::move(init), std::move(op));
}

template <class Collection, class UnaryPred>
bool unordered_all_of(const Collection& collection, UnaryPred p) {
  auto ui = UnorderedIterable(collection);
  return std::all_of(ui.begin(), ui.end(), std::move(p));
}

template <class Collection, class UnaryPred>
bool unordered_any_of(const Collection& collection, UnaryPred p) {
  auto ui = UnorderedIterable(collection);
  return std::any_of(ui.begin(), ui.end(), std::move(p));
}

template <class Collection, class UnaryPred>
bool unordered_none_of(const Collection& collection, UnaryPred p) {
  auto ui = UnorderedIterable(collection);
  return std::none_of(ui.begin(), ui.end(), std::move(p));
}

template <class Collection, class UnaryFunc>
UnaryFunc unordered_for_each(const Collection& collection, UnaryFunc f) {
  auto ui = UnorderedIterable(collection);
  std::for_each(ui.begin(), ui.end(), std::move(f));
  return f;
}

template <class Collection, class OutputIt>
OutputIt unordered_copy(const Collection& collection, OutputIt target) {
  auto ui = UnorderedIterable(collection);
  return std::copy(ui.begin(), ui.end(), target);
}

template <class Collection, class OutputIt, class UnaryPred>
OutputIt unordered_copy_if(const Collection& collection,
                           OutputIt target,
                           UnaryPred pred) {
  auto ui = UnorderedIterable(collection);
  return std::copy_if(ui.begin(), ui.end(), target, std::move(pred));
}

template <class Collection, class T>
typename Collection::difference_type unordered_count(
    const Collection& collection, const T& value) {
  auto ui = UnorderedIterable(collection);
  return std::count(ui.begin(), ui.end(), value);
}

template <class Collection, class UnaryPred>
typename Collection::difference_type unordered_count_if(
    const Collection& collection, UnaryPred pred) {
  auto ui = UnorderedIterable(collection);
  return std::count_if(ui.begin(), ui.end(), std::move(pred));
}

template <class Collection, typename Pred>
size_t unordered_erase_if(Collection& collection, const Pred& pred) {
  size_t removed = 0;
  auto ui = UnorderedIterable(collection);
  for (auto it = ui.begin(), end = ui.end(); it != end;) {
    if (pred(*it)) {
      it = ui.erase(it);
      removed++;
    } else {
      ++it;
    }
  }
  return removed;
}

template <class Collection, class OutputIt, class UnaryOp>
OutputIt unordered_transform(const Collection& collection,
                             OutputIt target,
                             UnaryOp unary_op) {
  auto ui = UnorderedIterable(collection);
  return std::transform(ui.begin(), ui.end(), target, std::move(unary_op));
}

template <class Target, class Source>
void insert_unordered_iterable(Target& target, const Source& source) {
  auto ui = UnorderedIterable(source);
  target.insert(ui.begin(), ui.end());
}

template <class Target, class TargetIt, class Source>
void insert_unordered_iterable(Target& target,
                               const TargetIt& target_it,
                               const Source& source) {
  auto ui = UnorderedIterable(source);
  target.insert(target_it, ui.begin(), ui.end());
}

template <class Collection>
struct UnorderedMergeContainers {
  void operator()(const Collection& addend, Collection* accumulator) {
    insert_unordered_iterable(*accumulator, addend);
  }
};
