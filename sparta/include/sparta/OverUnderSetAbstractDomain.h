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

  /* Returns elements in the over-approximation, excluding elements in the
   * under-approximation. */
  const Set& over() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->over();
  }

  /* Returns elements in the under-approximation. */
  const Set& under() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->under();
  }

  /* Returns all elements (union of over and under approximation). */
  Set elements() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->elements();
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
          /* under */ Set{}));
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

  explicit OverUnderSetValue(Element e) : m_under{std::move(e)}, m_over{} {}

  explicit OverUnderSetValue(std::initializer_list<Element> l)
      : m_under{l}, m_over{} {}

  explicit OverUnderSetValue(Set under) : m_under(std::move(under)), m_over{} {}

  OverUnderSetValue(Set over, Set under)
      : m_under(std::move(under)), m_over(std::move(over)) {
    // Enforce the class invariant.
    m_over.difference_with(m_under);
  }

  void clear() {
    m_under.clear();
    m_over.clear();
  }

  AbstractValueKind kind() const { return AbstractValueKind::Value; }

  bool empty() const { return m_under.empty() && m_over.empty(); }

  const Set& under() const { return m_under; }

  const Set& over() const { return m_over; }

  Set elements() const { return m_under.get_union_with(m_over); }

  void add_over(const Element& e) {
    if (!m_under.contains(e)) {
      m_over.insert(e);
    }
  }

  void add_over(Set set) {
    set.difference_with(m_under);
    m_over.union_with(set);
  }

  void add_under(const Element& e) {
    m_under.insert(e);
    m_over.remove(e);
  }

  void add_under(const Set& set) {
    m_under.union_with(set);
    m_over.difference_with(set);
  }

  void add(const OverUnderSetValue& other) {
    m_under.union_with(other.m_under);
    m_over.union_with(other.m_over);

    // Preserve the class invariant.
    m_over.difference_with(m_under);
  }

  bool leq(const OverUnderSetValue& other) const {
    // b_under ⊆ a_under
    if (!other.m_under.is_subset_of(m_under)) {
      return false;
    }

    // a_over* ⊆ b_over* = (a_over ∪ a_under) ⊆ (b_over ∪ b_under)
    Set other_elements = other.elements();
    return m_under.is_subset_of(other_elements) &&
           m_over.is_subset_of(other_elements);
  }

  bool equals(const OverUnderSetValue& other) const {
    return m_over.equals(other.m_over) && m_under.equals(other.m_under);
  }

  AbstractValueKind join_with(const OverUnderSetValue& other) {
    /*
     * The mathematical operation (with the over set including the under set,
     * annotated `*` here) - is:
     *  result_under = a_under ∩ b_under
     *  result_over* = a_over* ∪ b_over*
     *
     * This gives us:
     *  result_under = a_under ∩ b_under (unchanged) (1)
     *  result_over = (a_over ∪ a_under ∪ b_over ∪ b_under) - result_under
     *              = a_over - result_under (2)
     *              ∪ b_over - result_under (3)
     *              ∪ a_under - result_under (4)
     *              ∪ b_under - result_under (5)
     *
     * Also note that:
     *  a_over - result_under = a_over (2) : since a_over is disjoint with
     *  a_under, hence disjoint with result_under.
     *  b_over - result_under = b_over (3) with the same logic.
     */
    Set original_under = m_under;

    m_under.intersection_with(other.m_under); // (1)
    m_over.union_with(other.m_over); // (2) ∪ (3)

    // Add under elements that became over elements.
    m_over.union_with(original_under.get_difference_with(m_under)); // (4)
    m_over.union_with(other.m_under.get_difference_with(m_under)); // (5)

    return AbstractValueKind::Value;
  }

  AbstractValueKind widen_with(const OverUnderSetValue& other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const OverUnderSetValue& other) {
    /*
     * The mathematical operation (with the over set including the under set,
     * annotated `*` here) - is:
     *  result_under = a_under ∪ b_under
     *  result_over* = a_over* ∩ b_over*
     *
     * This gives us:
     *  result_under = a_under ∪ b_under (unchanged) (1)
     *  result_over = (
     *                 (a_over ∪ a_under) (2)
     *                 ∩ (b_over ∪ b_under) (3)
     *                )
     *                - result_under (4)
     */
    m_over.union_with(m_under); // (2)
    m_over.intersection_with(other.m_over.get_union_with(other.m_under)); // (3)
    m_under.union_with(other.m_under); // (1)

    if (!m_under.is_subset_of(m_over)) {
      return AbstractValueKind::Bottom;
    }

    // Preserve the class invariant.
    m_over.difference_with(m_under); // (4)
    return AbstractValueKind::Value;
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
  // Invariant: m_under.get_intersection_with(m_over).empty()
  Set m_under;
  Set m_over;
};

} // namespace over_under_set_impl
} // namespace sparta
