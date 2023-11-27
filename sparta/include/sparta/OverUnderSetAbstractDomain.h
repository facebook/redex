/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>

#include <sparta/AbstractDomain.h>
#include <sparta/AbstractSet.h>
#include <sparta/Exceptions.h>

namespace sparta {

namespace over_under_set_impl {

template <typename Set>
class OverUnderSetValue;

} // namespace over_under_set_impl

/**
 * An implementation of powerset abstract domains that computes both an over-
 * and under-approximation using the given set.
 */
template <typename Set>
class OverUnderSetAbstractDomain final
    : public AbstractDomainScaffolding<
          over_under_set_impl::OverUnderSetValue<Set>,
          OverUnderSetAbstractDomain<Set>> {
 public:
  using Value = over_under_set_impl::OverUnderSetValue<Set>;
  using Element = typename Set::value_type;

  /* Return the empty over-under set. */
  OverUnderSetAbstractDomain() { this->set_to_value(Value()); }

  explicit OverUnderSetAbstractDomain(AbstractValueKind kind)
      : AbstractDomainScaffolding<over_under_set_impl::OverUnderSetValue<Set>,
                                  OverUnderSetAbstractDomain<Set>>(kind) {}

  explicit OverUnderSetAbstractDomain(Element e) {
    this->set_to_value(Value(std::move(e)));
  }

  explicit OverUnderSetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  explicit OverUnderSetAbstractDomain(Set set) {
    this->set_to_value(Value(std::move(set)));
  }

  explicit OverUnderSetAbstractDomain(Set over, Set under) {
    this->set_to_value(Value(std::move(over), std::move(under)));
  }

  static OverUnderSetAbstractDomain bottom() {
    return OverUnderSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static OverUnderSetAbstractDomain top() {
    return OverUnderSetAbstractDomain(AbstractValueKind::Top);
  }

  bool empty() const { return this->is_value() && this->get_value()->empty(); }

  const Set& over() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->over();
  }

  const Set& under() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->under();
  }

  void add_over(const Element& e) { add_over_internal(e); }

  void add_over(const Set& set) { add_over_internal(set); }

  void add_over(Set&& set) { add_over_internal(std::move(set)); }

  void add_under(const Element& e) { add_under_internal(e); }

  void add_under(const Set& set) { add_under_internal(set); }

  void add_under(Set&& set) { add_under_internal(std::move(set)); }

  void add(const OverUnderSetAbstractDomain& other) {
    if (this->is_top() || other.is_bottom()) {
      return;
    } else if (other.is_top()) {
      this->set_to_top();
    } else if (this->is_bottom()) {
      this->set_to_value(*other.get_value());
    } else {
      this->get_value()->add(*other.get_value());
    }
  }

  friend std::ostream& operator<<(std::ostream& out,
                                  const OverUnderSetAbstractDomain& x) {
    using namespace sparta;
    switch (x.kind()) {
    case AbstractValueKind::Bottom: {
      out << "_|_";
      break;
    }
    case AbstractValueKind::Top: {
      out << "T";
      break;
    }
    case AbstractValueKind::Value: {
      out << *x.get_value();
      break;
    }
    }
    return out;
  }

 private:
  template <typename T>
  void add_over_internal(T&& element_or_set) {
    if (this->is_value()) {
      this->get_value()->add_over(std::forward<T>(element_or_set));
    } else if (this->is_bottom()) {
      this->set_to_value(Value(
          /* over */ Set(std::forward<T>(element_or_set)),
          /* under */ {}));
    }
  }

  template <typename T>
  void add_under_internal(T&& element_or_set) {
    if (this->is_value()) {
      this->get_value()->add_under(std::forward<T>(element_or_set));
    } else if (this->is_bottom()) {
      this->set_to_value(Value(std::forward<T>(element_or_set)));
    }
  }
};

namespace over_under_set_impl {

template <typename Set>
class OverUnderSetValue final : public AbstractValue<OverUnderSetValue<Set>> {
 public:
  using Element = typename Set::value_type;

  OverUnderSetValue() = default;

  explicit OverUnderSetValue(Element e)
      : OverUnderSetValue(Set{std::move(e)}) {}

  explicit OverUnderSetValue(std::initializer_list<Element> l)
      : OverUnderSetValue(Set(l)) {}

  explicit OverUnderSetValue(Set over_and_under)
      : m_over(over_and_under), m_under(std::move(over_and_under)) {
    // Union is unnecessary.
  }

  OverUnderSetValue(Set over, Set under)
      : m_over(std::move(over)), m_under(std::move(under)) {
    m_over.union_with(m_under);
  }

  void clear() {
    m_over.clear();
    m_under.clear();
  }

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool empty() const { return m_over.empty(); }

  const Set& over() const { return m_over; }

  const Set& under() const { return m_under; }

  void add_over(const Element& e) { m_over.insert(e); }

  void add_over(const Set& set) { m_over.union_with(set); }

  void add_under(const Element& e) {
    m_over.insert(e);
    m_under.insert(e);
  }

  void add_under(const Set& set) {
    m_over.union_with(set);
    m_under.union_with(set);
  }

  void add(const OverUnderSetValue& other) {
    m_over.union_with(other.m_over);
    m_under.union_with(other.m_under);
  }

  bool leq(const OverUnderSetValue& other) const {
    return m_over.is_subset_of(other.m_over) &&
           other.m_under.is_subset_of(m_under);
  }

  bool equals(const OverUnderSetValue& other) const {
    return m_over.equals(other.m_over) && m_under.equals(other.m_under);
  }

  AbstractValueKind join_with(const OverUnderSetValue& other) {
    m_over.union_with(other.m_over);
    m_under.intersection_with(other.m_under);
    return AbstractValueKind::Value;
  }

  AbstractValueKind widen_with(const OverUnderSetValue& other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const OverUnderSetValue& other) {
    m_over.intersection_with(other.m_over);
    m_under.union_with(other.m_under);
    return m_under.is_subset_of(m_over) ? AbstractValueKind::Value
                                        : AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const OverUnderSetValue& other) {
    return meet_with(other);
  }

  friend std::ostream& operator<<(std::ostream& out,
                                  const OverUnderSetValue& value) {
    if (value.empty()) {
      out << "{}";
    } else {
      out << "{over=" << value.m_over << ", under=" << value.m_under << "}";
    }
    return out;
  }

 private:
  // Invariant: m_under.is_subset_of(m_over)
  Set m_over;
  Set m_under;
};

} // namespace over_under_set_impl
} // namespace sparta
