/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>

#include "AbstractDomain.h"
#include "Exceptions.h"
#include "PatriciaTreeSet.h"

namespace sparta {

namespace ptousad_impl {

template <typename Element>
class OverUnderSetValue final
    : public AbstractValue<OverUnderSetValue<Element>> {
 public:
  OverUnderSetValue() = default;

  OverUnderSetValue(Element e)
      : OverUnderSetValue(PatriciaTreeSet<Element>{std::move(e)}) {}

  OverUnderSetValue(std::initializer_list<Element> l)
      : OverUnderSetValue(PatriciaTreeSet<Element>(l)) {}

  OverUnderSetValue(PatriciaTreeSet<Element> over_and_under)
      : m_over(over_and_under), m_under(std::move(over_and_under)) {
    // Union is unnecessary.
  }

  OverUnderSetValue(PatriciaTreeSet<Element> over,
                    PatriciaTreeSet<Element> under)
      : m_over(std::move(over)), m_under(std::move(under)) {
    m_over.union_with(m_under);
  }

  void clear() {
    m_over.clear();
    m_under.clear();
  }

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool empty() const { return m_over.empty(); }

  const PatriciaTreeSet<Element>& over() const { return m_over; }

  const PatriciaTreeSet<Element>& under() const { return m_under; }

  void add_over(Element e) { m_over.insert(std::move(e)); }

  void add_over(const PatriciaTreeSet<Element>& set) { m_over.union_with(set); }

  void add_under(Element e) {
    m_over.insert(e);
    m_under.insert(std::move(e));
  }

  void add_under(const PatriciaTreeSet<Element>& set) {
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
  PatriciaTreeSet<Element> m_over;
  PatriciaTreeSet<Element> m_under;
};

} // namespace ptousad_impl

/**
 * An implementation of powerset abstract domains that computes both an over-
 * and under-approximation using Patricia trees.
 *
 * This domain can only handle elements that are either unsigned integers or
 * pointers to objects.
 */
template <typename Element>
class PatriciaTreeOverUnderSetAbstractDomain final
    : public AbstractDomainScaffolding<
          ptousad_impl::OverUnderSetValue<Element>,
          PatriciaTreeOverUnderSetAbstractDomain<Element>> {
 public:
  using Value = ptousad_impl::OverUnderSetValue<Element>;

  /* Return the empty over-under set. */
  PatriciaTreeOverUnderSetAbstractDomain() { this->set_to_value(Value()); }

  explicit PatriciaTreeOverUnderSetAbstractDomain(AbstractValueKind kind)
      : AbstractDomainScaffolding<
            ptousad_impl::OverUnderSetValue<Element>,
            PatriciaTreeOverUnderSetAbstractDomain<Element>>(kind) {}

  explicit PatriciaTreeOverUnderSetAbstractDomain(Element e) {
    this->set_to_value(Value(std::move(e)));
  }

  explicit PatriciaTreeOverUnderSetAbstractDomain(
      std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  explicit PatriciaTreeOverUnderSetAbstractDomain(
      PatriciaTreeSet<Element> set) {
    this->set_to_value(Value(std::move(set)));
  }

  explicit PatriciaTreeOverUnderSetAbstractDomain(
      PatriciaTreeSet<Element> over, PatriciaTreeSet<Element> under) {
    this->set_to_value(Value(std::move(over), std::move(under)));
  }

  static PatriciaTreeOverUnderSetAbstractDomain bottom() {
    return PatriciaTreeOverUnderSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static PatriciaTreeOverUnderSetAbstractDomain top() {
    return PatriciaTreeOverUnderSetAbstractDomain(AbstractValueKind::Top);
  }

  bool empty() const { return this->is_value() && this->get_value()->empty(); }

  const PatriciaTreeSet<Element>& over() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->over();
  }

  const PatriciaTreeSet<Element>& under() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->under();
  }

  void add_over(Element e) { add_over_internal(std::move(e)); }

  void add_over(const PatriciaTreeSet<Element>& set) { add_over_internal(set); }

  void add_over(PatriciaTreeSet<Element>&& set) {
    add_over_internal(std::move(set));
  }

  void add_under(Element e) { add_under_internal(std::move(e)); }

  void add_under(const PatriciaTreeSet<Element>& set) {
    add_under_internal(set);
  }

  void add_under(PatriciaTreeSet<Element>&& set) {
    add_under_internal(std::move(set));
  }

  void add(const PatriciaTreeOverUnderSetAbstractDomain& other) {
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

  friend std::ostream& operator<<(
      std::ostream& out, const PatriciaTreeOverUnderSetAbstractDomain& x) {
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
          /* over */ PatriciaTreeSet<Element>(std::forward<T>(element_or_set)),
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

} // namespace sparta
