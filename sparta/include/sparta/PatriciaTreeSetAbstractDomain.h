/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <initializer_list>
#include <unordered_set>

#include <sparta/PatriciaTreeSet.h>
#include <sparta/PowersetAbstractDomain.h>

namespace sparta {

template <typename Element>
class PatriciaTreeSetAbstractDomain;

namespace ptsad_impl {

/*
 * An abstract value from a powerset is implemented as a Patricia tree.
 */
template <typename Element>
class SetValue final
    : public PowersetImplementation<Element,
                                    const PatriciaTreeSet<Element>&,
                                    SetValue<Element>> {
 public:
  SetValue() = default;

  explicit SetValue(Element e) : m_set(std::move(e)) {}

  explicit SetValue(std::initializer_list<Element> l)
      : m_set(l.begin(), l.end()) {}

  explicit SetValue(PatriciaTreeSet<Element> set) : m_set(std::move(set)) {}

  const PatriciaTreeSet<Element>& elements() const { return m_set; }

  size_t size() const { return m_set.size(); }

  bool contains(const Element& e) const { return m_set.contains(e); }

  void add(const Element& e) { m_set.insert(e); }

  void add(Element&& e) { m_set.insert(std::move(e)); }

  void remove(const Element& e) { m_set.remove(e); }

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

  friend std::ostream& operator<<(std::ostream& o, const SetValue& value) {
    o << "[#" << value.size() << "]";
    o << value.m_set;
    return o;
  }

 private:
  PatriciaTreeSet<Element> m_set;

  template <typename T>
  friend class sparta::PatriciaTreeSetAbstractDomain;
};

} // namespace ptsad_impl

/*
 * An implementation of powerset abstract domains using Patricia trees. This
 * implementation should be used for analyses that create large numbers of
 * identical or nearly identical sets (like a pointer analysis, for example).
 * This powerset domain can only handle elements that are either unsigned
 * integers or pointers to objects.
 *
 * Sample usage:
 *
 *  using Powerset = PatriciaTreeSetAbstractDomain<std::string*>;
 *
 *  std::string a = "a";
 *  ...
 *  Powerset s;
 *
 *  s.add(&a);
 *  ...
 *  for(std::string* p : s) {
 *    if (*p == "a") {
 *      ...
 *    }
 *  }
 *
 */
template <typename Element>
class PatriciaTreeSetAbstractDomain final
    : public PowersetAbstractDomain<Element,
                                    ptsad_impl::SetValue<Element>,
                                    const PatriciaTreeSet<Element>&,
                                    PatriciaTreeSetAbstractDomain<Element>> {
 public:
  using Value = ptsad_impl::SetValue<Element>;

  PatriciaTreeSetAbstractDomain()
      : PowersetAbstractDomain<Element,
                               Value,
                               const PatriciaTreeSet<Element>&,
                               PatriciaTreeSetAbstractDomain>() {}

  explicit PatriciaTreeSetAbstractDomain(AbstractValueKind kind)
      : PowersetAbstractDomain<Element,
                               Value,
                               const PatriciaTreeSet<Element>&,
                               PatriciaTreeSetAbstractDomain>(kind) {}

  explicit PatriciaTreeSetAbstractDomain(Element e) {
    this->set_to_value(Value(std::move(e)));
  }

  explicit PatriciaTreeSetAbstractDomain(std::initializer_list<Element> l) {
    this->set_to_value(Value(l));
  }

  explicit PatriciaTreeSetAbstractDomain(PatriciaTreeSet<Element> set) {
    this->set_to_value(Value(std::move(set)));
  }

  static PatriciaTreeSetAbstractDomain bottom() {
    return PatriciaTreeSetAbstractDomain(AbstractValueKind::Bottom);
  }

  static PatriciaTreeSetAbstractDomain top() {
    return PatriciaTreeSetAbstractDomain(AbstractValueKind::Top);
  }
};

} // namespace sparta
