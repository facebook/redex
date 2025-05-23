/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>

#include <sparta/AbstractDomain.h>
#include <sparta/Exceptions.h>
#include <sparta/FlatSet.h>

namespace sparta {

namespace sssad_impl {

template <typename Element, std::size_t MaxCount>
class SetValue final : public AbstractValue<SetValue<Element, MaxCount>> {
 public:
  SetValue() = default;

  explicit SetValue(FlatSet<Element> set) : m_set(std::move(set)) {}

  void clear() { m_set.clear(); }

  AbstractValueKind kind() const {
    if (m_set.size() > MaxCount) {
      return AbstractValueKind::Top;
    } else {
      return AbstractValueKind::Value;
    }
  }

  bool empty() const { return m_set.empty(); }

  std::size_t size() const { return m_set.size(); }

  const FlatSet<Element>& elements() const { return m_set; }

  void add(const Element& e) { m_set.insert(e); }

  void remove(const Element& e) { m_set.remove(e); }

  template <typename Predicate>
  void filter(Predicate&& predicate) const {
    m_set.filter(std::forward<Predicate>(predicate));
  }

  bool contains(const Element& e) const { return m_set.contains(e); }

  bool leq(const SetValue& other) const {
    return m_set.is_subset_of(other.m_set);
  }

  bool equals(const SetValue& other) const { return m_set.equals(other.m_set); }

  AbstractValueKind join_with(const SetValue& other) {
    m_set.union_with(other.m_set);
    return kind();
  }

  AbstractValueKind widen_with(const SetValue& other) {
    return join_with(other);
  }

  AbstractValueKind meet_with(const SetValue& other) {
    m_set.intersection_with(other.m_set);
    return kind();
  }

  AbstractValueKind narrow_with(const SetValue& other) {
    return meet_with(other);
  }

  friend std::ostream& operator<<(std::ostream& out, const SetValue& value) {
    out << value.m_set;
    return out;
  }

 private:
  FlatSet<Element> m_set;
};

} // namespace sssad_impl

/**
 * An implementation of powerset abstract domains with a maximum number of
 * elements based on a sorted vector. This implementation is optimized for small
 * sets, e.g `MaxCount <= 20`. When a set goes beyond `MaxCount` elements, it is
 * collapsed to top.
 */
template <typename Element, std::size_t MaxCount>
class SmallSortedSetAbstractDomain final
    : public AbstractDomainScaffolding<
          sssad_impl::SetValue<Element, MaxCount>,
          SmallSortedSetAbstractDomain<Element, MaxCount>> {
 public:
  using Value = sssad_impl::SetValue<Element, MaxCount>;

  /* Return the empty set. */
  SmallSortedSetAbstractDomain() { this->set_to_value(Value()); }

  explicit SmallSortedSetAbstractDomain(AbstractValueKind kind)
      : AbstractDomainScaffolding<
            sssad_impl::SetValue<Element, MaxCount>,
            SmallSortedSetAbstractDomain<Element, MaxCount>>(kind) {}

  explicit SmallSortedSetAbstractDomain(const Element& e) {
    this->set_to_value(Value(FlatSet<Element>{e}));
  }

  explicit SmallSortedSetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(FlatSet<Element>(l)));
  }

  explicit SmallSortedSetAbstractDomain(FlatSet<Element> set) {
    this->set_to_value(Value(std::move(set)));
  }

  static SmallSortedSetAbstractDomain bottom() {
    return SmallSortedSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static SmallSortedSetAbstractDomain top() {
    return SmallSortedSetAbstractDomain(AbstractValueKind::Top);
  }

  bool empty() const { return this->is_value() && this->get_value()->empty(); }

  const FlatSet<Element>& elements() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->elements();
  }

  std::size_t size() const {
    SPARTA_RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                         invalid_abstract_value()
                             << expected_kind(AbstractValueKind::Value)
                             << actual_kind(this->kind()));
    return this->get_value()->size();
  }

  void add(const Element& e) {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->add(e);
      this->normalize();
    }
  }

  void remove(const Element& e) {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->remove(e);
      this->normalize();
    }
  }

  template <typename Predicate>
  void filter(Predicate&& predicate) const {
    if (this->kind() == AbstractValueKind::Value) {
      this->get_value()->filter(std::forward<Predicate>(predicate));
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
      return this->get_value()->contains(e);
    }
    }
    SPARTA_RUNTIME_CHECK(
        false, internal_error() << error_msg("unknown AbstractValueKind"));
    // Return false to suppress -Wreturn-type warning reported by gcc
    return false;
  }

  friend std::ostream& operator<<(std::ostream& out,
                                  const SmallSortedSetAbstractDomain& x) {
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
