/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  OverUnderSetValue(PatriciaTreeSet<Element> over,
                    PatriciaTreeSet<Element> under)
      : m_over(std::move(over)), m_under(std::move(under)) {
    m_over.union_with(m_under);
  }

  void clear() override {
    m_over.clear();
    m_under.clear();
  }

  AbstractValueKind kind() const override { return AbstractValueKind::Value; }

  bool empty() const { return m_over.empty(); }

  const PatriciaTreeSet<Element>& over() const { return m_over; }

  const PatriciaTreeSet<Element>& under() const { return m_under; }

  void add_over(const PatriciaTreeSet<Element>& set) { m_over.union_with(set); }

  void add_under(const PatriciaTreeSet<Element>& set) {
    m_over.union_with(set);
    m_under.union_with(set);
  }

  void add(const OverUnderSetValue& other) {
    m_over.union_with(other.m_over);
    m_under.union_with(other.m_under);
  }

  bool leq(const OverUnderSetValue& other) const override {
    return m_over.is_subset_of(other.m_over) &&
           other.m_under.is_subset_of(m_under);
  }

  bool equals(const OverUnderSetValue& other) const override {
    return m_over.equals(other.m_over) && m_under.equals(other.m_under);
  }

  AbstractValueKind join_with(const OverUnderSetValue& other) override {
    m_over.union_with(other.m_over);
    m_under.intersection_with(other.m_under);
    return AbstractValueKind::Value;
  }

  AbstractValueKind widen_with(const OverUnderSetValue& other) override {
    return join_with(other);
  }

  AbstractValueKind meet_with(const OverUnderSetValue& other) override {
    m_over.intersection_with(other.m_over);
    m_under.union_with(other.m_under);
    return m_under.is_subset_of(m_over) ? AbstractValueKind::Value
                                        : AbstractValueKind::Bottom;
  }

  AbstractValueKind narrow_with(const OverUnderSetValue& other) override {
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

  explicit PatriciaTreeOverUnderSetAbstractDomain(const Element& e) {
    auto set = PatriciaTreeSet<Element>{e};
    this->set_to_value(Value(/* over */ set, /* under */ set));
  }

  explicit PatriciaTreeOverUnderSetAbstractDomain(
      std::initializer_list<Element> l) {
    auto set = PatriciaTreeSet<Element>(l);
    this->set_to_value(Value(/* over */ set, /* under */ set));
  }

  explicit PatriciaTreeOverUnderSetAbstractDomain(
      const PatriciaTreeSet<Element>& set) {
    this->set_to_value(Value(/* over */ set, /* under */ set));
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

  void add_over(const Element& e) { add_over(PatriciaTreeSet<Element>{e}); }

  void add_over(const PatriciaTreeSet<Element>& set) {
    if (this->is_value()) {
      this->get_value()->add_over(set);
    }
  }

  void add_under(const Element& e) { add_under(PatriciaTreeSet<Element>{e}); }

  void add_under(const PatriciaTreeSet<Element>& set) {
    if (this->is_value()) {
      this->get_value()->add_under(set);
    }
  }

  void add(const PatriciaTreeOverUnderSetAbstractDomain& other) {
    if (this->is_bottom()) {
      return;
    } else if (other.is_bottom()) {
      this->set_to_bottom();
    } else if (this->is_top()) {
      return;
    } else if (other.is_top()) {
      this->set_to_top();
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
};

} // namespace sparta
