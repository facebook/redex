/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <unordered_set>

#include "PowersetAbstractDomain.h"

namespace sparta {

// Forward declaration.
template <typename Element, typename Hash, typename Equal>
class HashedSetAbstractDomain;

namespace hsad_impl {

/*
 * An abstract value from a powerset is implemented as a hash table.
 */
template <typename Element, typename Hash, typename Equal>
class SetValue final : public PowersetImplementation<
                           Element,
                           const std::unordered_set<Element, Hash, Equal>&,
                           SetValue<Element, Hash, Equal>> {
 public:
  using SetImplType = std::unordered_set<Element, Hash, Equal>;

  SetValue() = default;

  SetValue(const Element& e) { set().insert(e); }

  SetValue(std::initializer_list<Element> l) {
    if (l.begin() != l.end()) {
      m_set = std::make_unique<SetImplType>(l.begin(), l.end());
    }
  }

  SetValue(const SetValue& other) {
    if (other.m_set) {
      m_set = std::make_unique<SetImplType>(*other.m_set);
    }
  }

  SetValue(SetValue&& other) noexcept = default;

  SetValue& operator=(const SetValue& other) {
    if (other.m_set) {
      m_set = std::make_unique<SetImplType>(*other.m_set);
    }
    return *this;
  }

  SetValue& operator=(SetValue&& other) noexcept = default;

  const SetImplType& elements() const override {
    if (m_set) {
      return *m_set;
    } else {
      return s_empty_set;
    }
  }

  size_t size() const override { return m_set ? m_set->size() : 0; }

  bool contains(const Element& e) const override {
    return m_set && m_set->count(e) > 0;
  }

  void add(const Element& e) override { set().insert(e); }

  void remove(const Element& e) override {
    if (m_set) {
      if (m_set->erase(e) && m_set->empty()) {
        m_set = nullptr;
      }
    }
  }

  void clear() override { m_set = nullptr; }

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool leq(const SetValue& other) const override {
    if (size() > other.size()) {
      return false;
    }
    if (m_set) {
      for (const Element& e : *m_set) {
        if (other.contains(e) == 0) {
          return false;
        }
      }
    }
    return true;
  }

  bool equals(const SetValue& other) const override {
    return (size() == other.size()) && leq(other);
  }

  AbstractValueKind join_with(const SetValue& other) override {
    if (other.m_set) {
      auto& this_set = set();
      for (const Element& e : *other.m_set) {
        this_set.insert(e);
      }
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind meet_with(const SetValue& other) override {
    if (m_set) {
      for (auto it = m_set->begin(); it != m_set->end();) {
        if (other.contains(*it) == 0) {
          it = m_set->erase(it);
        } else {
          ++it;
        }
      }
    }
    return AbstractValueKind::Value;
  }

  AbstractValueKind difference_with(const SetValue& other) override {
    if (m_set) {
      for (auto it = m_set->begin(); it != m_set->end();) {
        if (other.contains(*it) != 0) {
          it = m_set->erase(it);
        } else {
          ++it;
        }
      }
    }
    return AbstractValueKind::Value;
  }

  friend std::ostream& operator<<(std::ostream& o, const SetValue& value) {
    o << "[#" << value.size() << "]";
    const auto& elements = value.elements();
    o << "{";
    for (auto it = elements.begin(); it != elements.end();) {
      o << *it++;
      if (it != elements.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  static inline const SetImplType s_empty_set{};
  std::unique_ptr<SetImplType> m_set;
  SetImplType& set() {
    if (!m_set) {
      m_set = std::make_unique<SetImplType>();
    }
    return *m_set;
  }

  template <typename T1, typename T2, typename T3>
  friend class sparta::HashedSetAbstractDomain;
};

} // namespace hsad_impl

/*
 * An implementation of powerset abstract domains using hash tables.
 */
template <typename Element,
          typename Hash = std::hash<Element>,
          typename Equal = std::equal_to<Element>>
class HashedSetAbstractDomain final
    : public PowersetAbstractDomain<
          Element,
          hsad_impl::SetValue<Element, Hash, Equal>,
          const std::unordered_set<Element, Hash, Equal>&,
          HashedSetAbstractDomain<Element, Hash, Equal>> {
 public:
  using Value = hsad_impl::SetValue<Element, Hash, Equal>;

  HashedSetAbstractDomain()
      : PowersetAbstractDomain<Element,
                               Value,
                               const std::unordered_set<Element, Hash, Equal>&,
                               HashedSetAbstractDomain>() {}

  HashedSetAbstractDomain(AbstractValueKind kind)
      : PowersetAbstractDomain<Element,
                               Value,
                               const std::unordered_set<Element, Hash, Equal>&,
                               HashedSetAbstractDomain>(kind) {}

  explicit HashedSetAbstractDomain(const Element& e) {
    this->set_to_value(Value(e));
  }

  explicit HashedSetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  static HashedSetAbstractDomain bottom() {
    return HashedSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static HashedSetAbstractDomain top() {
    return HashedSetAbstractDomain(AbstractValueKind::Top);
  }
};

} // namespace sparta
