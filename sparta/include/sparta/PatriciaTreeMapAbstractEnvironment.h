/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <utility>

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeMap.h>

namespace sparta {

namespace ptmae_impl {

template <typename Variable, typename Domain>
class MapValue;

class value_is_bottom {};

} // namespace ptmae_impl

/*
 * An abstract environment based on Patricia trees that is cheap to copy.
 *
 * In order to minimize the size of the underlying tree, we do not explicitly
 * represent bindings of a variable to the Top element.
 *
 * See HashedAbstractEnvironment.h for more details about abstract
 * environments.
 */
template <typename Variable, typename Domain>
class PatriciaTreeMapAbstractEnvironment final
    : public AbstractDomainScaffolding<
          ptmae_impl::MapValue<Variable, Domain>,
          PatriciaTreeMapAbstractEnvironment<Variable, Domain>> {
 public:
  using Value = ptmae_impl::MapValue<Variable, Domain>;

  using MapType =
      PatriciaTreeMap<Variable, Domain, typename Value::ValueInterface>;

  /*
   * The default constructor produces the Top value.
   */
  PatriciaTreeMapAbstractEnvironment()
      : AbstractDomainScaffolding<Value, PatriciaTreeMapAbstractEnvironment>() {
  }

  explicit PatriciaTreeMapAbstractEnvironment(AbstractValueKind kind)
      : AbstractDomainScaffolding<Value, PatriciaTreeMapAbstractEnvironment>(
            kind) {}

  explicit PatriciaTreeMapAbstractEnvironment(
      std::initializer_list<std::pair<Variable, Domain>> l) {
    for (const auto& p : l) {
      if (p.second.is_bottom()) {
        this->set_to_bottom();
        return;
      }
      this->get_value()->insert_binding(p.first, p.second);
    }
    this->normalize();
  }

  size_t size() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map.size();
  }

  const MapType& bindings() const {
    RUNTIME_CHECK(this->kind() == AbstractValueKind::Value,
                  invalid_abstract_value()
                      << expected_kind(AbstractValueKind::Value)
                      << actual_kind(this->kind()));
    return this->get_value()->m_map;
  }

  const Domain& get(const Variable& variable) const {
    if (this->is_bottom()) {
      static const Domain bottom = Domain::bottom();
      return bottom;
    }
    return this->get_value()->m_map.at(variable);
  }

  PatriciaTreeMapAbstractEnvironment& set(const Variable& variable,
                                          const Domain& value) {
    return set_internal(variable, value);
  }

  PatriciaTreeMapAbstractEnvironment& set(const Variable& variable,
                                          Domain&& value) {
    return set_internal(variable, std::move(value));
  }

  template <typename Operation> // Domain(const Domain&)
  bool transform(Operation&& f) {
    if (this->is_bottom()) {
      return false;
    }
    bool res = this->get_value()->transform(std::forward<Operation>(f));
    this->normalize();
    return res;
  }

  bool erase_all_matching(const Variable& variable_mask) {
    if (this->is_bottom()) {
      return false;
    }
    bool res = this->get_value()->erase_all_matching(variable_mask);
    this->normalize();
    return res;
  }

  PatriciaTreeMapAbstractEnvironment& clear() {
    if (this->is_bottom()) {
      return *this;
    }
    this->get_value()->clear();
    this->normalize();
    return *this;
  }

  template <typename Operation> // Domain(const Domain&)
  PatriciaTreeMapAbstractEnvironment& update(const Variable& variable,
                                             Operation&& operation) {
    if (this->is_bottom()) {
      return *this;
    }
    try {
      this->get_value()->m_map.update(
          [operation = std::forward<Operation>(operation)](const Domain& x) {
            Domain result = operation(x);
            if (result.is_bottom()) {
              throw ptmae_impl::value_is_bottom();
            }
            return result;
          },
          variable);
    } catch (const ptmae_impl::value_is_bottom&) {
      this->set_to_bottom();
    }
    this->normalize();
    return *this;
  }

  static PatriciaTreeMapAbstractEnvironment bottom() {
    return PatriciaTreeMapAbstractEnvironment(AbstractValueKind::Bottom);
  }

  static PatriciaTreeMapAbstractEnvironment top() {
    return PatriciaTreeMapAbstractEnvironment(AbstractValueKind::Top);
  }

 private:
  template <typename D>
  PatriciaTreeMapAbstractEnvironment& set_internal(const Variable& variable,
                                                   D&& value) {
    if (this->is_bottom()) {
      return *this;
    }
    if (value.is_bottom()) {
      this->set_to_bottom();
      return *this;
    }
    this->get_value()->insert_binding(variable, std::forward<D>(value));
    this->normalize();
    return *this;
  }
};

} // namespace sparta

template <typename Variable, typename Domain>
inline std::ostream& operator<<(
    std::ostream& o,
    const typename sparta::PatriciaTreeMapAbstractEnvironment<Variable, Domain>&
        e) {
  using namespace sparta;
  switch (e.kind()) {
  case AbstractValueKind::Bottom: {
    o << "_|_";
    break;
  }
  case AbstractValueKind::Top: {
    o << "T";
    break;
  }
  case AbstractValueKind::Value: {
    o << "[#" << e.size() << "]";
    o << e.bindings();
    break;
  }
  }
  return o;
}

namespace sparta {

namespace ptmae_impl {

/*
 * The definition of an element of an abstract environment, i.e., a map from a
 * (possibly infinite) set of variables to an abstract domain implemented as a
 * hashtable. Variable bindings with the Top value are not stored in the
 * hashtable. The hashtable can never contain bindings with Bottom, as those are
 * filtered out in PatriciaTreeMapAbstractEnvironment (the whole environment is
 * set to Bottom in that case). The Meet and Narrowing operations abort and
 * return AbstractValueKind::Bottom whenever a binding with Bottom is about to
 * be created.
 */
template <typename Variable, typename Domain>
class MapValue final : public AbstractValue<MapValue<Variable, Domain>> {
 public:
  struct ValueInterface {
    using type = Domain;

    static type default_value() { return type::top(); }

    static bool is_default_value(const type& x) { return x.is_top(); }

    static bool equals(const type& x, const type& y) { return x.equals(y); }

    static bool leq(const type& x, const type& y) { return x.leq(y); }

    constexpr static AbstractValueKind default_value_kind =
        AbstractValueKind::Top;
  };

  MapValue() = default;

  MapValue(const Variable& variable, Domain value) {
    insert_binding(variable, std::move(value));
  }

  void clear() { m_map.clear(); }

  AbstractValueKind kind() const {
    // If the map is empty, then all variables are implicitly bound to Top,
    // i.e., the abstract environment itself is Top.
    return m_map.empty() ? AbstractValueKind::Top : AbstractValueKind::Value;
  }

  bool leq(const MapValue& other) const { return m_map.leq(other.m_map); }

  bool equals(const MapValue& other) const { return m_map.equals(other.m_map); }

  AbstractValueKind join_with(const MapValue& other) {
    return join_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.join(y); });
  }

  AbstractValueKind widen_with(const MapValue& other) {
    return join_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.widening(y); });
  }

  AbstractValueKind meet_with(const MapValue& other) {
    return meet_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.meet(y); });
  }

  AbstractValueKind narrow_with(const MapValue& other) {
    return meet_like_operation(
        other, [](const Domain& x, const Domain& y) { return x.narrowing(y); });
  }

 private:
  void insert_binding(const Variable& variable, Domain value) {
    // The Bottom value is handled by the caller and should never occur here.
    RUNTIME_CHECK(!value.is_bottom(), internal_error());
    m_map.insert_or_assign(variable, std::move(value));
  }

  template <typename Operation> // Domain(const Domain&)
  bool transform(Operation&& f) {
    return m_map.transform(std::forward<Operation>(f));
  }

  bool erase_all_matching(const Variable& variable_mask) {
    return m_map.erase_all_matching(variable_mask);
  }

  template <typename Operation> // Domain(const Domain&, const Domain&)
  AbstractValueKind join_like_operation(const MapValue& other,
                                        Operation&& operation) {
    m_map.intersection_with(std::forward<Operation>(operation), other.m_map);
    return kind();
  }

  template <typename Operation> // Domain(const Domain&, const Domain&)
  AbstractValueKind meet_like_operation(const MapValue& other,
                                        Operation&& operation) {
    try {
      m_map.union_with(
          [operation = std::forward<Operation>(operation)](const Domain& x,
                                                           const Domain& y) {
            Domain result = operation(x, y);
            if (result.is_bottom()) {
              throw value_is_bottom();
            }
            return result;
          },
          other.m_map);
      return kind();
    } catch (const value_is_bottom&) {
      clear();
      return AbstractValueKind::Bottom;
    }
  }

  PatriciaTreeMap<Variable, Domain, ValueInterface> m_map;

  template <typename T1, typename T2>
  friend class sparta::PatriciaTreeMapAbstractEnvironment;
};

} // namespace ptmae_impl

} // namespace sparta
