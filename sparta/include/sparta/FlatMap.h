/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <vector>

#include <boost/iterator/transform_iterator.hpp>

#include <sparta/PatriciaTreeCore.h>

namespace sparta {

/*
 * Represents a map implemented with a sorted vector.
 *
 * It is similar to `boost::container::flat_map` but provides map operations
 * such as union and intersection, using the same interface as
 * `PatriciaTreeMap`.
 */
template <typename Key,
          typename ValueType,
          typename Value = pt_core::SimpleValue<ValueType>,
          typename KeyCompare = std::less<Key>,
          typename KeyEqual = std::equal_to<Key>>
class FlatMap final {
 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename Value::type;
  using value_type = std::pair<Key, mapped_type>;
  using iterator = typename std::vector<value_type>::const_iterator;
  using const_iterator = iterator;
  using difference_type = std::ptrdiff_t;
  using size_type = size_t;
  using const_reference = const value_type&;
  using const_pointer = const value_type*;

  static_assert(std::is_same_v<ValueType, mapped_type>,
                "ValueType must be equal to Value::type");

 private:
  struct ComparePairWithKey {
    bool operator()(const value_type& pair, const Key& key) const {
      return KeyCompare()(pair.first, key);
    }
  };

  struct PairEqual {
    bool operator()(const value_type& left, const value_type& right) const {
      return KeyEqual()(left.first, right.first) &&
             Value::equals(left.second, right.second);
    }
  };

  typename std::vector<value_type>::iterator find_lower_bound(const Key& key) {
    return std::lower_bound(m_vector.begin(), m_vector.end(), key,
                            ComparePairWithKey());
  }

  typename std::vector<value_type>::const_iterator find_lower_bound(
      const Key& key) const {
    // Use `const_cast<>` to avoid duplicate code generation.
    return const_cast<FlatMap*>(this)->find_lower_bound(key);
  }

  typename std::vector<value_type>::iterator find(const Key& key) {
    auto it = this->find_lower_bound(key);
    if (it == m_vector.end() || !KeyEqual()(it->first, key)) {
      return m_vector.end();
    }
    return it;
  }

  typename std::vector<value_type>::const_iterator find(const Key& key) const {
    // Use `const_cast<>` to avoid duplicate code generation.
    return const_cast<FlatMap*>(this)->find(key);
  }

  void erase_default_values() {
    m_vector.erase(std::remove_if(m_vector.begin(),
                                  m_vector.end(),
                                  [](const value_type& p) {
                                    return Value::is_default_value(p.second);
                                  }),
                   m_vector.end());
  }

 public:
  explicit FlatMap() = default;

  explicit FlatMap(std::initializer_list<std::pair<Key, ValueType>> l) {
    for (const auto& p : l) {
      insert_or_assign(p.first, p.second);
    }
  }

  bool empty() const { return m_vector.empty(); }

  size_t size() const { return m_vector.size(); }

  size_t max_size() const { return m_vector.max_size(); }

  iterator begin() const { return m_vector.cbegin(); }

  iterator end() const { return m_vector.cend(); }

  const mapped_type& at(const Key& key) const {
    auto it = find(key);
    if (it == m_vector.end()) {
      static const ValueType default_value = Value::default_value();
      return default_value;
    } else {
      return it->second;
    }
  }

  FlatMap& remove(const Key& key) {
    auto it = find(key);
    if (it != m_vector.end()) {
      m_vector.erase(it);
    }
    return *this;
  }

  FlatMap& insert_or_assign(const Key& key, mapped_type value) {
    if (Value::is_default_value(value)) {
      return remove(key);
    }

    auto it = this->find_lower_bound(key);
    if (it == m_vector.end() || !KeyEqual()(it->first, key)) {
      m_vector.emplace(it, key, std::move(value));
    } else {
      it->second = std::move(value);
    }
    return *this;
  }

  template <typename Operation> // void(mapped_type*)
  FlatMap& update(Operation&& operation, const Key& key) {
    auto it = this->find_lower_bound(key);
    bool existing = it != m_vector.end() && KeyEqual()(it->first, key);

    ValueType new_value = Value::default_value();
    ValueType* value = existing ? &it->second : &new_value;

    operation(value);
    if (Value::is_default_value(*value)) {
      if (existing) {
        m_vector.erase(it);
      }
    } else {
      if (!existing) {
        m_vector.emplace(it, key, std::move(*value));
      }
    }

    return *this;
  }

 private:
  bool leq_when_default_is_top(const FlatMap& other) const {
    if (m_vector.size() < other.m_vector.size()) {
      // In this case, there is a key bound to a non-Top value in 'other'
      // that is not defined in 'this' (and is therefore implicitly bound to
      // Top).
      return false;
    }

    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (other_it != other_end) {
      if (std::distance(it, end) < std::distance(other_it, other_end)) {
        // Same logic as above: there is a key bound to a non-Top value between
        // [other_it, other_end] that is not defined within [it, end].
        return false;
      }
      // Performs a binary search (in O(log(n))) which returs an iterator on the
      // first pair where `it->first >= other_it->first`.
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey());
      if (it == end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!Value::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }
    return true;
  }

  bool leq_when_default_is_bottom(const FlatMap& other) const {
    if (m_vector.size() > other.m_vector.size()) {
      // `this` has at least one non-default binding that `other` doesn't have.
      // There exists a key such that this[key] != Bottom and other[key] ==
      // Bottom.
      return false;
    }

    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (it != end) {
      if (std::distance(it, end) > std::distance(other_it, other_end)) {
        // Same logic as above: there is a non-default binding in [it, end]
        // that does not exist in [it, end], hence is bound to bottom.
        return false;
      }
      // Performs a binary search (in O(log(n))) which returs an iterator on the
      // first pair where `other_it->first >= it->first`.
      other_it = std::lower_bound(other_it, other_end, it->first,
                                  ComparePairWithKey());
      if (other_it == other_end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!Value::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }

    return true;
  }

 public:
  bool leq(const FlatMap& other) const {
    static_assert(std::is_base_of_v<AbstractDomain<ValueType>, ValueType>,
                  "leq can only be used when Value implements AbstractDomain");

    // Assumes Value::default_value() is either Top or Bottom.
    if (Value::default_value().is_top()) {
      return this->leq_when_default_is_top(other);
    } else if (Value::default_value().is_bottom()) {
      return this->leq_when_default_is_bottom(other);
    } else {
      RUNTIME_CHECK(false, undefined_operation());
    }
  }

  bool equals(const FlatMap& other) const {
    return std::equal(m_vector.begin(), m_vector.end(), other.m_vector.begin(),
                      other.m_vector.end(), PairEqual());
  }

  friend bool operator==(const FlatMap& m1, const FlatMap& m2) {
    return m1.equals(m2);
  }

  friend bool operator!=(const FlatMap& m1, const FlatMap& m2) {
    return !m1.equals(m2);
  }

  template <typename MappingFunction> // void(mapped_type*)
  void map(MappingFunction&& f) {
    bool has_default_value = false;
    for (auto& p : m_vector) {
      f(&p.second);
      if (Value::is_default_value(p.second)) {
        has_default_value = true;
      }
    }
    if (has_default_value) {
      erase_default_values();
    }
  }

  template <typename Predicate> // bool(const Key&, const ValueType&)
  FlatMap& filter(Predicate&& predicate) {
    m_vector.erase(std::remove_if(m_vector.begin(),
                                  m_vector.end(),
                                  [predicate = std::forward<Predicate>(
                                       predicate)](const value_type& p) {
                                    return !predicate(p.first, p.second);
                                  }),
                   m_vector.end());
    return *this;
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  void union_with(const CombiningFunction& combine, const FlatMap& other) {
    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey());
      if (it == end) {
        m_vector.insert(m_vector.end(), other_it, other_end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
      } else {
        it = m_vector.insert(it, *other_it);
        end = m_vector.end();
      }
      ++it;
      ++other_it;
    }
    erase_default_values();
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  void intersection_with(const CombiningFunction& combine,
                         const FlatMap& other) {
    auto it = m_vector.begin(), end = m_vector.end();
    auto other_it = other.m_vector.begin(), other_end = other.m_vector.end();
    while (it != end) {
      other_it = std::lower_bound(other_it, other_end, it->first,
                                  ComparePairWithKey());
      if (other_it == other_end) {
        m_vector.erase(it, end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
        ++it;
        ++other_it;
      } else {
        it->second = Value::default_value();
        ++it;
      }
    }
    erase_default_values();
  }

  void clear() { m_vector.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const FlatMap& m) {
    using namespace sparta;
    o << "{";
    for (auto it = m.begin(); it != m.end(); ++it) {
      o << pt_util::deref(it->first) << " -> " << it->second;
      if (std::next(it) != m.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  std::vector<value_type> m_vector;
};

} // namespace sparta
