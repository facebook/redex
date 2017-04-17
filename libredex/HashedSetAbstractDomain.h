/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_set>

#include "AbstractDomain.h"
#include "Debug.h"

template <typename Element, typename Hash, typename Equal>
class HashedSetAbstractDomain;

namespace hsad_impl {

/*
 * The definition of an abstract value belonging to a powerset abstract domain,
 * i.e., a set of elements implemented using a hashtable.
 */
template <typename Element, typename Hash, typename Equal>
class SetValue final : public AbstractValue<SetValue<Element, Hash, Equal>> {
 public:
  using Kind = typename AbstractValue<SetValue<Element, Hash, Equal>>::Kind;

  SetValue() = default;

  SetValue(const Element& e) { m_set.insert(e); }

  SetValue(std::initializer_list<Element> l) : m_set(l.begin(), l.end()) {}

  void clear() override { m_set.clear(); }

  Kind kind() const override { return Kind::Value; }

  bool leq(const SetValue& other) const override {
    if (m_set.size() > other.m_set.size()) {
      return false;
    }
    for (const Element& e : m_set) {
      if (other.m_set.count(e) == 0) {
        return false;
      }
    }
    return true;
  }

  bool equals(const SetValue& other) const override {
    return (m_set.size() == other.m_set.size()) && leq(other);
  }

  Kind join_with(const SetValue& other) override {
    for (const Element& e : other.m_set) {
      m_set.insert(e);
    }
    return Kind::Value;
  }

  Kind widen_with(const SetValue& other) override { return join_with(other); }

  Kind meet_with(const SetValue& other) override {
    for (auto it = m_set.begin(); it != m_set.end();) {
      if (other.m_set.count(*it) == 0) {
        it = m_set.erase(it);
      } else {
        ++it;
      }
    }
    return Kind::Value;
  }

  Kind narrow_with(const SetValue& other) override { return meet_with(other); }

 private:
  std::unordered_set<Element, Hash, Equal> m_set;

  template <typename T1, typename T2, typename T3>
  friend class ::HashedSetAbstractDomain;
};

} // namespace hsad_impl

/*
 * An implementation of a powerset abstract domain using hash tables. A powerset
 * abstract domain is the complete lattice made of all subsets of a base set of
 * elements. Note that in this abstract domain Bottom is different from the
 * empty set. Bottom represents an unreachable program configuration, whereas
 * the empty set may have a perfectly valid semantics (like in liveness
 * analysis or pointer analysis, for example). Since in practice the base set of
 * elements is usually very large or infinite, it is implicitly represented by
 * Top. We use the AbstractDomainScaffolding template to build the domain.
 */
template <typename Element,
          typename Hash = std::hash<Element>,
          typename Equal = std::equal_to<Element>>
class HashedSetAbstractDomain final
    : public AbstractDomainScaffolding<
          hsad_impl::SetValue<Element, Hash, Equal>,
          HashedSetAbstractDomain<Element, Hash, Equal>> {
 public:
  using Value = hsad_impl::SetValue<Element, Hash, Equal>;

  using AbstractValueKind = typename AbstractValue<Value>::Kind;

  /*
   * This constructor produces the empty set, which is distinct from Bottom.
   */
  HashedSetAbstractDomain()
      : AbstractDomainScaffolding<Value, HashedSetAbstractDomain>() {}

  HashedSetAbstractDomain(AbstractValueKind kind)
      : AbstractDomainScaffolding<Value, HashedSetAbstractDomain>(kind) {}

  explicit HashedSetAbstractDomain(const Element& e) {
    this->set_to_value(Value(e));
  }

  explicit HashedSetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  size_t size() const {
    assert(this->kind() == AbstractValueKind::Value);
    return this->get_value()->m_set.size();
  }

  const std::unordered_set<Element, Hash, Equal>& elements() const {
    assert(this->kind() == AbstractValueKind::Value);
    return this->get_value()->m_set;
  }

  void add(const Element& e) {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->m_set.insert(e);
    }
  }

  void add(std::initializer_list<Element> l) {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->m_set.insert(l);
    }
  }

  template <typename InputIterator>
  void add(InputIterator first, InputIterator last) {
    if (this->kind() == AbstractValueKind::Value) {
      for (InputIterator it = first; it != last; ++it) {
        this->get_value()->m_set.insert(*it);
      }
    }
  }

  void remove(const Element& e) {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->m_set.erase(e);
    }
  }

  void remove(std::initializer_list<Element> l) {
    if (this->kind() == AbstractValueKind::Value) {
      for (const auto& e : l) {
        this->get_value()->m_set.erase(e);
      }
    }
  }

  template <typename InputIterator>
  void remove(InputIterator first, InputIterator last) {
    if (this->kind() == AbstractValueKind::Value) {
      for (InputIterator it = first; it != last; ++it) {
        this->get_value()->m_set.erase(*it);
      }
    }
  }

  bool contains(const Element& e) const {
    switch (this->kind()) {
    case AbstractValueKind::Bottom: {
      return false;
    }
    case AbstractValueKind::Top: {
      return true;
    }
    case AbstractValueKind::Value: {
      return this->get_value()->m_set.count(e) > 0;
    }
    }
  }

  static HashedSetAbstractDomain bottom() {
    return HashedSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static HashedSetAbstractDomain top() {
    return HashedSetAbstractDomain(AbstractValueKind::Top);
  }
};

template <typename Element, typename Hash, typename Equal>
inline std::ostream& operator<<(
    std::ostream& o, const HashedSetAbstractDomain<Element, Hash, Equal>& s) {
  using AbstractValueKind =
      typename HashedSetAbstractDomain<Element, Hash, Equal>::AbstractValueKind;
  switch (s.kind()) {
  case AbstractValueKind::Bottom: {
    o << "_|_";
    break;
  }
  case AbstractValueKind::Top: {
    o << "T";
    break;
  }
  case AbstractValueKind::Value: {
    o << "[#" << s.size() << "]";
    o << "{";
    auto& elements = s.elements();
    for (auto it = elements.begin(); it != elements.end();) {
      o << *it++;
      if (it != elements.end()) {
        o << ", ";
      }
    }
    o << "}";
    break;
  }
  }
  return o;
}
