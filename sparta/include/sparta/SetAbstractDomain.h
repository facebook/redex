/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <initializer_list>

#include <sparta/AbstractSet.h>
#include <sparta/PowersetAbstractDomain.h>

namespace sparta {

namespace set_impl {

template <typename Set>
class SetAbstractDomainStaticAssert;
template <typename Set>
class SetValue;

} // namespace set_impl

/*
 * An implementation of powerset abstract domains based on the given set.
 */
template <typename Set>
class SetAbstractDomain final
    : public PowersetAbstractDomain<typename Set::value_type,
                                    set_impl::SetValue<Set>,
                                    const Set&,
                                    SetAbstractDomain<Set>>,
      private set_impl::SetAbstractDomainStaticAssert<Set> {
 public:
  using Value = set_impl::SetValue<Set>;
  using Element = typename Set::value_type;

  SetAbstractDomain()
      : PowersetAbstractDomain<Element,
                               Value,
                               const Set&,
                               SetAbstractDomain>() {}

  explicit SetAbstractDomain(AbstractValueKind kind)
      : PowersetAbstractDomain<Element, Value, const Set&, SetAbstractDomain>(
            kind) {}

  explicit SetAbstractDomain(Element e) {
    this->set_to_value(Value(std::move(e)));
  }

  explicit SetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  explicit SetAbstractDomain(Set set) {
    this->set_to_value(Value(std::move(set)));
  }

  static SetAbstractDomain bottom() {
    return SetAbstractDomain(AbstractValueKind::Bottom);
  }

  static SetAbstractDomain top() {
    return SetAbstractDomain(AbstractValueKind::Top);
  }
};

namespace set_impl {

template <typename Set>
class SetAbstractDomainStaticAssert {
 protected:
  ~SetAbstractDomainStaticAssert() {
    static_assert(std::is_base_of<AbstractSet<Set>, Set>::value,
                  "Set doesn't inherit from AbstractSet");
  }
};

template <typename Set>
class SetValue final : public PowersetImplementation<typename Set::value_type,
                                                     const Set&,
                                                     SetValue<Set>> {
 public:
  using Element = typename Set::value_type;

  SetValue() = default;

  explicit SetValue(Element e) : m_set(std::move(e)) {}

  explicit SetValue(std::initializer_list<Element> l)
      : m_set(l.begin(), l.end()) {}

  explicit SetValue(Set set) : m_set(std::move(set)) {}

  const Set& elements() const { return m_set; }

  bool empty() const { return m_set.empty(); }

  size_t size() const { return m_set.size(); }

  bool contains(const Element& e) const { return m_set.contains(e); }

  void add(const Element& e) { m_set.insert(e); }

  void add(Element&& e) { m_set.insert(std::move(e)); }

  void remove(const Element& e) { m_set.remove(e); }

  template <typename Predicate>
  void filter(Predicate&& predicate) {
    m_set.filter(std::forward<Predicate>(predicate));
  }

  void clear() { m_set.clear(); }

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool leq(const SetValue& other) const {
    return m_set.is_subset_of(other.m_set);
  }

  bool equals(const SetValue& other) const { return m_set.equals(other.m_set); }

  AbstractValueKind join_with(const SetValue& other) {
    m_set.union_with(other.m_set);
    return AbstractValueKind::Value;
  }

  AbstractValueKind meet_with(const SetValue& other) {
    m_set.intersection_with(other.m_set);
    return AbstractValueKind::Value;
  }

  AbstractValueKind difference_with(const SetValue& other) {
    m_set.difference_with(other.m_set);
    return AbstractValueKind::Value;
  }

  AbstractValueKind erase_all_matching(const Element& variable_mask) {
    m_set.erase_all_matching(variable_mask);
    return AbstractValueKind::Value;
  }

  friend std::ostream& operator<<(std::ostream& o, const SetValue& value) {
    o << "[#" << value.size() << "]";
    o << value.m_set;
    return o;
  }

 private:
  Set m_set;

  friend class sparta::SetAbstractDomain<Set>;
};

} // namespace set_impl

} // namespace sparta
