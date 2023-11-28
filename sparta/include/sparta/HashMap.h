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
#include <unordered_map>

#include <sparta/AbstractMap.h>
#include <sparta/AbstractMapValue.h>
#include <sparta/PatriciaTreeCore.h>

namespace sparta {
namespace hm_impl {
template <typename Key,
          typename Value,
          typename ValueInterface,
          typename KeyHash,
          typename KeyEqual>
class HashMapStaticAssert;
}

/*
 * A hash map.
 *
 * It is similar to `std::unordered_map` but provides map operations
 * such as union and intersection, using the same interface as
 * `PatriciaTreeMap`.
 */
template <typename Key,
          typename Value,
          typename ValueInterface = pt_core::SimpleValue<Value>,
          typename KeyHash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class HashMap final
    : public AbstractMap<
          HashMap<Key, Value, ValueInterface, KeyHash, KeyEqual>>,
      private hm_impl::
          HashMapStaticAssert<Key, Value, ValueInterface, KeyHash, KeyEqual> {
 public:
  using StdUnorderedMap = std::unordered_map<Key, Value, KeyHash, KeyEqual>;

  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename ValueInterface::type;
  using value_type = typename StdUnorderedMap::value_type;
  using iterator = typename StdUnorderedMap::const_iterator;
  using const_iterator = iterator;
  using difference_type = typename StdUnorderedMap::difference_type;
  using size_type = typename StdUnorderedMap::size_type;
  using const_reference = typename StdUnorderedMap::const_reference;
  using const_pointer = typename StdUnorderedMap::const_pointer;

  using value_interface = ValueInterface;
  constexpr static AbstractMapMutability mutability =
      AbstractMapMutability::Mutable;

  explicit HashMap() = default;

  explicit HashMap(std::initializer_list<std::pair<Key, Value>> l) {
    for (const auto& p : l) {
      insert_or_assign(p.first, p.second);
    }
  }

  bool empty() const { return m_map.empty(); }

  size_t size() const { return m_map.size(); }

  size_t max_size() const { return m_map.max_size(); }

  iterator begin() const { return m_map.cbegin(); }

  iterator end() const { return m_map.cend(); }

  const mapped_type& at(const Key& key) const {
    auto it = m_map.find(key);
    if (it == m_map.end()) {
      static const Value default_value = ValueInterface::default_value();
      return default_value;
    } else {
      return it->second;
    }
  }

  HashMap& remove(const Key& key) {
    m_map.erase(key);
    return *this;
  }

  HashMap& insert_or_assign(const Key& key, const mapped_type& value) {
    if (ValueInterface::is_default_value(value)) {
      remove(key);
    } else {
      m_map.insert_or_assign(key, value);
    }
    return *this;
  }

  HashMap& insert_or_assign(const Key& key, mapped_type&& value) {
    if (ValueInterface::is_default_value(value)) {
      remove(key);
    } else {
      m_map.insert_or_assign(key, std::move(value));
    }
    return *this;
  }

  template <typename Operation> // void(mapped_type*)
  HashMap& update(Operation&& operation, const Key& key) {
    auto it = m_map.find(key);
    bool existing = it != m_map.end();

    Value new_value = ValueInterface::default_value();
    Value* value = existing ? &it->second : &new_value;

    operation(value);

    if (ValueInterface::is_default_value(*value)) {
      if (existing) {
        m_map.erase(it);
      }
    } else {
      if (!existing) {
        m_map.emplace(key, std::move(*value));
      }
    }

    return *this;
  }

 private:
  bool leq_when_default_is_top(const HashMap& other) const {
    if (m_map.size() < other.m_map.size()) {
      // In this case, there is a key bound to a non-Top value in 'other'
      // that is not defined in 'this' (and is therefore implicitly bound to
      // Top).
      return false;
    }

    for (const auto& binding : other.m_map) {
      auto it = m_map.find(binding.first);
      if (it == m_map.end()) {
        // The value is Top, but we know by construction that binding.second is
        // not Top.
        return false;
      }
      if (!ValueInterface::leq(it->second, binding.second)) {
        return false;
      }
    }

    return true;
  }

  bool leq_when_default_is_bottom(const HashMap& other) const {
    if (m_map.size() > other.m_map.size()) {
      // `this` has at least one non-default binding that `other` doesn't have.
      // There exists a key such that this[key] != Bottom and other[key] ==
      // Bottom.
      return false;
    }

    for (const auto& binding : m_map) {
      auto it = other.m_map.find(binding.first);
      if (it == other.m_map.end()) {
        // The other value is Bottom.
        return false;
      }
      if (!ValueInterface::leq(binding.second, it->second)) {
        return false;
      }
    }
    return true;
  }

 public:
  bool leq(const HashMap& other) const {
    static_assert(std::is_base_of_v<AbstractDomain<Value>, Value>,
                  "leq can only be used when Value implements AbstractDomain");

    // Assumes ValueInterface::default_value_kind is either Top or Bottom.
    if constexpr (ValueInterface::default_value_kind ==
                  AbstractValueKind::Top) {
      return this->leq_when_default_is_top(other);
    } else if constexpr (ValueInterface::default_value_kind ==
                         AbstractValueKind::Bottom) {
      return this->leq_when_default_is_bottom(other);
    } else {
      static_assert(
          ValueInterface::default_value_kind == AbstractValueKind::Top ||
              ValueInterface::default_value_kind == AbstractValueKind::Bottom,
          "leq can only be used when ValueInterface::default_value() is top or "
          "bottom");
    }
  }

  bool equals(const HashMap& other) const {
    if (m_map.size() != other.m_map.size()) {
      return false;
    }
    for (const auto& binding : m_map) {
      auto it = other.m_map.find(binding.first);
      if (it == other.m_map.end()) {
        return false;
      }
      if (!ValueInterface::equals(binding.second, it->second)) {
        return false;
      }
    }
    return true;
  }

  template <typename MappingFunction> // void(mapped_type*)
  void transform(MappingFunction&& f) {
    auto it = m_map.begin(), end = m_map.end();
    while (it != end) {
      f(&it->second);
      if (ValueInterface::is_default_value(it->second)) {
        it = m_map.erase(it);
      } else {
        ++it;
      }
    }
  }

  template <typename Visitor> // void(const value_type&)
  void visit(Visitor&& visitor) const {
    for (const auto& binding : m_map) {
      visitor(binding);
    }
  }

  template <typename Predicate> // bool(const Key&, const Value&)
  HashMap& filter(Predicate&& predicate) {
    auto it = m_map.begin(), end = m_map.end();
    while (it != end) {
      if (!predicate(it->first, it->second)) {
        it = m_map.erase(it);
      } else {
        ++it;
      }
    }
    return *this;
  }

  bool erase_all_matching(const Key& key_mask) {
    throw std::logic_error("not implemented");
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  HashMap& union_with(CombiningFunction&& combine, const HashMap& other) {
    for (const auto& other_binding : other.m_map) {
      auto binding = m_map.find(other_binding.first);
      if (binding == m_map.end()) {
        m_map.emplace(other_binding.first, other_binding.second);
      } else {
        combine(&binding->second, other_binding.second);
        if (ValueInterface::is_default_value(binding->second)) {
          m_map.erase(binding);
        }
      }
    }
    return *this;
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  HashMap& intersection_with(CombiningFunction&& combine,
                             const HashMap& other) {
    auto it = m_map.begin(), end = m_map.end();
    while (it != end) {
      auto other_binding = other.m_map.find(it->first);
      if (other_binding == other.m_map.end()) {
        it = m_map.erase(it);
      } else {
        combine(&it->second, other_binding->second);
        if (ValueInterface::is_default_value(it->second)) {
          it = m_map.erase(it);
        } else {
          ++it;
        }
      }
    }
    return *this;
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  // Requires `combine(bottom, ...)` to be a no-op.
  template <typename CombiningFunction>
  HashMap& difference_with(CombiningFunction&& combine, const HashMap& other) {
    for (const auto& other_binding : other.m_map) {
      auto binding = m_map.find(other_binding.first);
      if (binding != m_map.end()) {
        combine(&binding->second, other_binding.second);
        if (ValueInterface::is_default_value(binding->second)) {
          m_map.erase(binding);
        }
      }
    }
    return *this;
  }

  void clear() { m_map.clear(); }

  size_t count(const Key& key) const { return m_map.count(key); }

  friend std::ostream& operator<<(std::ostream& o, const HashMap& m) {
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
  StdUnorderedMap m_map;
};

namespace hm_impl {
template <typename Key,
          typename Value,
          typename ValueInterface,
          typename KeyHash,
          typename KeyEqual>
class HashMapStaticAssert {
 protected:
  ~HashMapStaticAssert() {
    static_assert(std::is_same_v<Value, typename ValueInterface::type>,
                  "Value must be equal to ValueInterface::type");
    static_assert(std::is_base_of<AbstractMapValue<ValueInterface>,
                                  ValueInterface>::value,
                  "ValueInterface doesn't inherit from AbstractMapValue");
    ValueInterface::check_interface();
  };
};
} // namespace hm_impl

} // namespace sparta
